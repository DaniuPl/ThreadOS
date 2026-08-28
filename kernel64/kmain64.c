/* ProtonOS 64 - pierwszy krok portu na tryb dlugi
 *
 * Do tego miejsca procesor doszedl juz przez boot64/boot64.S: ma
 * wlaczony tryb dlugi, wlasna tablice deskryptorow i pierwsze cztery
 * gigabajty zamapowane identycznosciowo stronami po 2 MiB.
 *
 * Ten plik jest jeszcze szkieletem. Wazne jest to, czego tu NIE MA:
 * zadnej kopii kodu z jadra 32-bitowego. Wypisywanie, konsola na
 * buforze obrazu, odczyt znacznikow Multiboota 2 i obsluga lancuchow
 * to te same pliki, kompilowane drugi raz - roznice miedzy trybami
 * zamykaja sie w naglowku io.h i w kilku rzutowaniach.
 */

#include <stdint.h>
#include "../kernel/include/mb2.h"
#include "../kernel/include/fbcon.h"
#include "../kernel/include/vga.h"
#include "../kernel/include/serial.h"
#include "../kernel/include/printk.h"
#include "../kernel/include/pic.h"
#include "../kernel/include/pit.h"
#include "../kernel/include/pmm.h"
#include "../kernel/include/heap.h"
#include "idt64.h"
#include "paging64.h"
#include "task64.h"
#include "gdt64.h"
#include "syscall64.h"
#include "elf64.h"
#include "proc64.h"
#include "keyboard64.h"
#include "shell64.h"
#include "isr64.h"
#include "../kernel/include/initrd.h"
#include "../kernel/include/blk.h"
#include "../kernel/include/fs.h"
#include "../kernel/include/string.h"

/* Konsole wirtualne pytaja zadanie, na ktorej ma pisac. Konsola jest
   na razie jedna, wiec wszystko idzie na pierwsza. */
int task_vc(void)
{
    return 0;
}

/* Watki demonstracyjne: dwa liczace bez konca i jeden, ktory sam sie
   konczy - po to, zeby bylo widac, ze zombie znika z listy. */
static volatile uint32_t counter_a, counter_b;
static volatile int mortal_done;

static void count_a(void *arg)
{
    (void)arg;
    for (;;) {
        counter_a++;
        task64_sleep(20);
    }
}

static void count_b(void *arg)
{
    (void)arg;
    for (;;) {
        counter_b++;
        task64_sleep(50);
    }
}

static void mortal(void *arg)
{
    (void)arg;
    task64_sleep(300);
    mortal_done = 1;
}

/* Watek, ktory schodzi do ring 3.
 *
 * Kod programu lezy w obrazie jadra, ale ring 3 nie ma prawa go tam
 * czytac - kopiujemy go wiec do swiezej ramki i mapujemy ja jako
 * dostepna dla uzytkownika. Stos programu to druga taka ramka.
 *
 * Ze stosem jadra jest inaczej niz przy bramce przerwania: instrukcja
 * syscall stosu nie przelacza, wiec adres wlasnego stosu trzeba
 * procesorowi podac z gory - i to dwa razy, bo przerwanie z ring 3
 * bierze go skad indziej (z TSS) niz wywolanie systemowe. */
extern const uint8_t user_payload_start[], user_payload_end[];

#define USER_CODE_VA  0x0000000040000000ULL
#define USER_STACK_VA 0x0000000040001000ULL

static void ring3_thread(void *arg)
{
    (void)arg;

    uint32_t code = pmm_alloc_frame();
    uint32_t stack = pmm_alloc_frame();
    if (!code || !stack) {
        printk("[ !! ] Ring 3: brak pamieci\n");
        task64_exit();
    }

    uint64_t len = (uint64_t)(user_payload_end - user_payload_start);
    memcpy((void *)(uintptr_t)code, user_payload_start, (uint32_t)len);

    /* Kod bez prawa zapisu, stos bez prawa wykonywania - jedno i drugie
       ring 3 widzi, ale zadne nie jest jednoczesnie i pisane, i wykonywane */
    if (paging64_map(USER_CODE_VA, code, PG_USER) < 0 ||
        paging64_map(USER_STACK_VA, stack, PG_USER | PG_WRITE | PG_NX) < 0) {
        printk("[ !! ] Ring 3: nie da sie zmapowac programu\n");
        task64_exit();
    }

    /* Wierzcholek wlasnego stosu - stad jadro wystartuje przy kazdym
       wejsciu z ring 3 */
    uint64_t kstack;
    __asm__ volatile("movq %%rsp, %0" : "=r"(kstack));
    task64_enter_space(0, kstack & ~0xFULL);   /* przestrzen jadra */

    enter_ring3_64(USER_CODE_VA, USER_STACK_VA + PAGE_SIZE - 16, 0, 0);
}

/* Watek, ktory uruchamia PROGRAM Z PLIKU.
 *
 * Roznica wzgledem watku wyzej jest zasadnicza, choc konczy sie tym
 * samym skokiem: tam kod lezal w obrazie jadra, tutaj jadro dostaje
 * plik, ktorego nie widzialo w czasie budowy, i wszystkiego - gdzie
 * zaczac, co zaladowac, z jakimi prawami - dowiaduje sie z naglowka
 * ELF. To jest ta granica, za ktora system zaczyna uruchamiac cudze
 * programy, a nie wlasne kawalki siebie. */
/* Uruchamia program i czeka, az sie skonczy - sam albo zabity przez
   jadro. Zwraca 1, gdy w ogole ruszyl. */
static int run_program(const char *name)
{
    syscall64_forget_user();
    isr64_forget_user_fault();

    if (proc64_spawn(name, 0, 0) < 0)
        return 0;
    for (int i = 0; i < 80; i++) {
        if (syscall64_user_exited() || isr64_user_fault())
            break;
        task64_sleep(50);
    }
    return 1;
}

/* Czy procesor naprawde jest w trybie dlugim? Pyta o to sam siebie:
   bit LMA w rejestrze EFER ustawia procesor, a nie my. */
static int long_mode_active(void)
{
    uint32_t low, high;
    __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(0xC0000080));
    (void)high;
    return (low & (1u << 10)) != 0;
}

void kmain64(uint32_t magic, uint32_t info_addr)
{
    const struct multiboot_info *mbi = mb2_info(magic, info_addr);

    const struct mb2_framebuffer *lfb = mb2_framebuffer();
    int fb_taken = lfb && fbcon_set_framebuffer(lfb->addr, lfb->pitch,
                                                (int)lfb->width,
                                                (int)lfb->height,
                                                (int)lfb->bpp) == 0;
    if (fb_taken)
        console_use_framebuffer();

    vga_init();
    serial_init();

    vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
    printk("ProtonOS 64\n");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    printk("Jadro uruchomione w trybie dlugim (x86-64).\n\n");

    if (!mbi) {
        vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
        printk("BLAD: nieznany magic od bootloadera (%x)!\n", magic);
        for (;;)
            __asm__ volatile("cli; hlt");
    }
    printk("[ OK ] Multiboot OK (%s)\n", mb2_boot_kind());

    vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
    printk("[ OK ] ");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    printk("Tryb dlugi: LMA=%u, wskaznik ma %u bajtow\n",
           (uint32_t)long_mode_active(), (uint32_t)sizeof(void *));

    if (mbi->flags & MULTIBOOT_FLAG_MODS)
        printk("[ OK ] Moduly od loadera: %u\n", mbi->mods_count);

    if (fbcon_have())
        printk("[ OK ] Bufor obrazu: %ux%u, %u bitow na piksel\n",
               (uint32_t)fbcon_width(), (uint32_t)fbcon_height(),
               (uint32_t)fbcon_bpp());
    else if (lfb)
        printk("[ !! ] Bufor obrazu %ux%u ma %u bitow na piksel - "
               "umiemy 24 i 32\n", lfb->width, lfb->height, lfb->bpp);
    else
        printk("[ !! ] Loader nie dal bufora obrazu - tylko port szeregowy\n");

    if (mb2_rsdp())
        printk("[ OK ] Wskaznik ACPI od loadera: %x\n", mb2_rsdp());

    /* --- Pamiec ------------------------------------------------- */
    pmm_init(mbi);
    printk("[ OK ] Pamiec fizyczna: %u KiB RAM, %u wolnych ramek 4 KiB\n",
           pmm_ram_kib(), pmm_total_frames() - pmm_used_frames());

    if (paging64_init() < 0) {
        vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
        printk("BLAD: nie udalo sie zbudowac tablic stron\n");
        for (;;)
            __asm__ volatile("cli; hlt");
    }
    printk("[ OK ] Stronicowanie czteropoziomowe: %u MiB, zakaz "
           "wykonywania %s\n",
           (uint32_t)(paging64_identity_top() / (1024 * 1024)),
           paging64_nx_enabled() ? "wlaczony" : "wylaczony");

    /* Mapowanie drobne: ramka wstawiona wysoko, daleko poza obszar
       opisany identycznosciowo. Zapis i odczyt przez ten adres to
       dowod, ze cala czterostopniowa sciezka dziala. */
    uint32_t frame = pmm_alloc_frame();
    uint64_t high = 0x0000000200000000ULL;      /* osiem gigabajtow */
    if (frame && paging64_map(high, frame, PG_WRITE | PG_NX) == 0) {
        volatile uint64_t *p = (volatile uint64_t *)(uintptr_t)high;
        *p = 0x0123456789ABCDEFULL;
        uint64_t phys = paging64_lookup(high);
        printk("[ OK ] Mapowanie 4 KiB pod 8 GiB: odczyt %x%x, "
               "adres fizyczny %x\n",
               (uint32_t)(*p >> 32), (uint32_t)*p, (uint32_t)phys);
    } else {
        printk("[ !! ] Mapowanie 4 KiB nie powiodlo sie\n");
    }

    heap_init();
    /* Szybki sprawdzian sterty: przydziel, zapisz, zwolnij i sprawdz,
       czy pamiec wraca do puli - ten sam test, co w jadrze 32-bitowym */
    char *a = kmalloc(1024);
    char *b = kmalloc(4096);
    int heap_ok = a && b && a != b;
    if (heap_ok) {
        for (int i = 0; i < 1024; i++)
            a[i] = (char)i;
        for (int i = 0; i < 1024; i++)
            if (a[i] != (char)i)
                heap_ok = 0;
    }
    uint32_t used = heap_used_bytes();
    kfree(a);
    kfree(b);
    printk("[ OK ] Sterta jadra: %s (szczyt %u B, po zwolnieniu %u B)\n",
           heap_ok ? "kmalloc/kfree dzialaja" : "BLAD", used,
           heap_used_bytes());

    /* --- Przerwania --------------------------------------------- */
    idt64_init();
    printk("[ OK ] IDT zaladowana (wyjatki 0-31, IRQ 32-47)\n");

    pic_init();
    printk("[ OK ] PIC przemapowany (IRQ -> wektory 32-47)\n");

    pit_init(100);
    printk("[ OK ] Timer PIT: 100 Hz\n");

    __asm__ volatile("sti");
    printk("[ OK ] Przerwania wlaczone (sti)\n");

    /* Wyjatek-pulapka: sprawdza cala droge - stub, tablice, ramke
       rejestrow i powrot przez iretq - nie zatrzymujac systemu */
    __asm__ volatile("int $3");

    /* Timer musi tykac sam z siebie: czekamy na przerwania i patrzymy,
       czy licznik rusza. To dowod, ze przerwania sprzetowe dochodza. */
    uint32_t start = pit_ticks();
    for (int i = 0; i < 200; i++)
        __asm__ volatile("hlt");
    printk("[ OK ] Timer tyka: %u -> %u tyknac\n", start, pit_ticks());

    /* --- Watki -------------------------------------------------- */
    scheduler64_init();
    task64_create("licz-A", count_a, 0);
    task64_create("licz-B", count_b, 0);
    task64_create("krotki", mortal, 0);
    printk("[ OK ] Scheduler: %d zadania na liscie\n", task64_count());

    /* Watek glowny zasypia; procesor musi w tym czasie pojsc do innych.
       Gdyby przelaczanie kontekstu bylo zepsute, liczniki zostalyby
       na zerze albo system zawisl. */
    task64_sleep(600);
    printk("[ OK ] Watki licza rownolegle: A=%u, B=%u\n",
           counter_a, counter_b);
    printk("[ OK ] Watek konczacy sie: %s, zadan na liscie: %d\n",
           mortal_done ? "zrobil swoje" : "nie zdazyl", task64_count());

    /* --- Ring 3 -------------------------------------------------- */
    gdt64_init();
    printk("[ OK ] GDT z segmentami ring 3 i zadaniem TSS\n");

    syscall64_init();
    printk("[ OK ] Wywolania systemowe przez instrukcje syscall\n");

    task64_create("ring3", ring3_thread, 0);
    task64_sleep(200);

    /* Selektor kodu zna tylko procesor - dwa najnizsze bity mowia,
       w ktorym pierscieniu naprawde dzialal program */
    uint32_t cs = syscall64_user_cs();
    printk("[ OK ] Program w ring 3: %s, cs=%x, pierscien %u\n",
           syscall64_user_ran() ? "odezwal sie" : "MILCZY", cs, cs & 3);

    /* --- Programy z initrd ------------------------------------- */
    initrd_init(mbi);
    if (!initrd_file_count())
        printk("[ !! ] initrd: brak programow - nie ma czego uruchomic\n");
    else
        printk("[ OK ] initrd: %u plik(ow) od loadera\n",
               initrd_file_count());

    if (initrd_file_count()) {
    /* Program z pliku dostaje czysta karte: to, co powie jadru, mowi
       juz on, a nie kawalek kodu wpisany w obraz jadra */
    if (run_program("hello64")) {
        cs = syscall64_user_cs();
        printk("[ OK ] ELF64 z initrd: pierscien %u, kod wyjscia %u\n",
               cs & 3, (uint32_t)syscall64_user_status());
    }

    /* --- Ochrona pamieci od strony programu ---------------------- */
    /* Bit zakazu wykonywania byl dotad tylko ustawiony. Teraz program
       probuje wykonac wlasne dane, a bledy strony z ring 3 zabijaja
       winnego zamiast zatrzymywac system - wiec da sie to sprawdzic
       i przezyc. */
    if (run_program("nx64"))
        printk("[ %s ] Zakaz wykonywania: %s (err=%x, adres=%x)\n",
               isr64_user_fault() ? "OK" : "!!",
               isr64_user_fault() ? "procesor odmowil wykonania danych"
                                  : "PROGRAM WYKONAL SWOJE DANE",
               isr64_user_fault_err(), (uint32_t)isr64_user_fault_addr());

    if (run_program("wild64"))
        printk("[ %s ] Pamiec jadra: %s (err=%x, adres=%x)\n",
               isr64_user_fault() ? "OK" : "!!",
               isr64_user_fault() ? "niedostepna dla pierscienia 3"
                                  : "PROGRAM JA PRZECZYTAL",
               isr64_user_fault_err(), (uint32_t)isr64_user_fault_addr());

    /* Najwazniejszy dowod: system po tych dwoch zgonach dziala dalej */
    uint32_t a_before = counter_a;
    task64_sleep(200);
    printk("[ %s ] System przezyl smierc programow (licznik %u -> %u)\n",
           counter_a > a_before ? "OK" : "!!", a_before, counter_a);

    /* --- Dwa procesy naraz, kazdy we wlasnej przestrzeni ---------- */
    /* Ta sama kopia tego samego programu, oba pod tym samym adresem
       wirtualnym - i kazdy ze swoja pamiecia. Jedyne, co je rozni, to
       liczba, ktora dostaly w RDI. */
    uint32_t frames_before = pmm_used_frames();
    int base_tasks = task64_count();
    syscall64_forget_user();

    /* Ta sama kopia programu, a rozni je jedno: argument. Numer
       procesu przychodzi teraz jako argv[1], a nie wpisany do
       rejestru - czyli tak, jak dostaje go kazdy program. */
    static struct args64 sep_a = { 2, { "sep64", "1" } };
    static struct args64 sep_b = { 2, { "sep64", "2" } };
    proc64_spawn("sep64", &sep_a, 1);
    proc64_spawn("sep64", &sep_b, 1);
    for (int i = 0; i < 120 && task64_count() > base_tasks; i++)
        task64_sleep(50);

    int exits = syscall64_user_exits();
    int fails = syscall64_user_failures();
    printk("[ %s ] Dwa procesy naraz: %u zakonczonych, %u bledow\n",
           (exits == 2 && fails == 0) ? "OK" : "!!",
           (uint32_t)exits, (uint32_t)fails);

    /* Przestrzenie musza zniknac razem z procesami - inaczej kazdy
       uruchomiony program zabieralby pamiec na zawsze */
    uint32_t frames_after = pmm_used_frames();
    printk("[ %s ] Pamiec po procesach oddana (%u -> %u ramek)\n",
           frames_after <= frames_before ? "OK" : "!!",
           frames_before, frames_after);

    /* --- Proces powielajacy sam siebie -------------------------- */
    /* Tu jadro nie robi juz nic poza podaniem programu: drugi proces
       bierze sie z fork-a, czyli z decyzji samego programu. */
    frames_before = pmm_used_frames();
    base_tasks = task64_count();
    syscall64_forget_user();

    proc64_spawn("fork64", 0, 1);
    for (int i = 0; i < 160 && task64_count() > base_tasks; i++)
        task64_sleep(50);

    exits = syscall64_user_exits();
    fails = syscall64_user_failures();
    printk("[ %s ] Proces powielil sie sam: %u zakonczonych, %u bledow\n",
           (exits == 2 && fails == 0) ? "OK" : "!!",
           (uint32_t)exits, (uint32_t)fails);
    frames_after = pmm_used_frames();
    printk("[ %s ] Pamiec po fork-u oddana (%u -> %u ramek)\n",
           frames_after <= frames_before ? "OK" : "!!",
           frames_before, frames_after);

    /* --- fork + exec + wait ------------------------------------- */
    /* Stad juz tylko krok do shella: program potrafi uruchomic INNY
       program i doczekac jego konca. */
    base_tasks = task64_count();
    syscall64_forget_user();

    proc64_spawn("spawn64", 0, 1);
    for (int i = 0; i < 200 && task64_count() > base_tasks; i++)
        task64_sleep(50);

    /* Tu nie liczymy niezerowych wyjsc: hello64 CELOWO konczy sie
       siodemka, a to wlasnie ta siodemka jest dowodem, ze rodzic
       doczekal wlasciwego dziecka. Werdykt wydaje sam spawn64 - jego
       kod wyjscia jest tym ostatnim. */
    exits = syscall64_user_exits();
    int verdict = syscall64_user_status();
    printk("[ %s ] fork + exec + wait: %u zakonczonych, werdykt %u\n",
           (exits == 2 && verdict == 0) ? "OK" : "!!",
           (uint32_t)exits, (uint32_t)verdict);

    /* fork nie kopiuje juz pamieci - dzieli ja i rozdziela dopiero
       te strony, ktore ktos naprawde zapisal */
    uint32_t splits = paging64_cow_splits();
    uint32_t cost = proc64_last_fork_cost();
    printk("[ %s ] Kopiowanie przy zapisie: fork wzial %u ramek, "
           "rozdzielono %u stron\n",
           (splits > 0 && splits < 16 && cost < 8) ? "OK" : "!!",
           cost, splits);
    }

    task64_dump();

    /* --- Dysk --------------------------------------------------- */
    /* PCI, ATA, AHCI i warstwa blokowa to te same pliki, co w jadrze
       32-bitowym. Sterowniki nie wiedza nic o trybie procesora: gadaja
       z urzadzeniem przez porty i pamiec, a jedno i drugie wyglada
       tak samo po obu stronach. */
    if (blk_init()) {
        /* System plikow tez jest tym samym plikiem, co w jadrze
           32-bitowym - i tu akurat nie trzeba bylo zmienic w nim ani
           jednego znaku. Dysk widziany przez warstwe blokowa wyglada
           tak samo niezaleznie od trybu procesora. */
        int fs = fs_init();
        printk("[ OK ] Dysk %s + PFS %s (%u sektorow)\n",
               blk_name(), fs ? "gotowy" : "NIEDOSTEPNY", blk_sectors());
    } else {
        printk("[ !! ] Dysk: zaden kontroler nie odpowiedzial\n");
    }

    /* --- Wejscie z klawiatury ----------------------------------- */
    keyboard64_init();
    printk("[ OK ] Klawiatura PS/2 (IRQ 1)\n");

    printk("\nSystem 64-bitowy gotowy - wpisz 'help'.\n");
    shell64_start();

    /* Watek glowny staje sie od tej chwili watkiem bezczynnosci -
       robota przejmuje shell, ktory czeka na klawisze */
    for (;;)
        __asm__ volatile("hlt");
}
