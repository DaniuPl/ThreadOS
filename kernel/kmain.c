/* ProtonOS - the kernel's main entry point in C */

#include <stdint.h>
#include "include/vga.h"
#include "include/video.h"
#include "include/serial.h"
#include "include/printk.h"
#include "include/gdt.h"
#include "include/idt.h"
#include "include/pic.h"
#include "include/pit.h"
#include "include/keyboard.h"
#include "include/mouse.h"
#include "include/multiboot.h"
#include "include/pmm.h"
#include "include/paging.h"
#include "include/tlb.h"
#include "include/config.h"
#include "include/heap.h"
#include "include/shell.h"
#include "include/string.h"
#include "include/task.h"
#include "include/initrd.h"
#include "include/fs.h"
#include "include/blk.h"
#include "include/hda.h"
#include "include/acpi.h"
#include "include/mb2.h"
#include "include/usbkbd.h"
#include "include/usbmouse.h"
#include "include/bt.h"
#include "include/dynlink.h"
#include "include/biglock.h"
#include "include/swap.h"
#include "include/desktop.h"
#include "include/net.h"
#include "include/netdev.h"
#include "include/smp.h"
#include "include/dhcp.h"
#include "include/tcp.h"

#define MULTIBOOT_BOOTLOADER_MAGIC 0x2BADB002

/* Does the kernel command line contain a given word?
   QEMU passes it with the -append option, GRUB with an entry in
   grub.cfg. */
static int cmdline_has(const struct multiboot_info *mbi, const char *word)
{
    if (!(mbi->flags & MULTIBOOT_FLAG_CMDLINE) || !mbi->cmdline)
        return 0;

    const char *cmd = (const char *)mbi->cmdline;
    size_t wlen = strlen(word);

    for (int i = 0; cmd[i]; i++) {
        if (i && cmd[i - 1] != ' ')
            continue;                    /* match whole words only */
        size_t k = 0;
        while (k < wlen && cmd[i + k] == word[k])
            k++;
        if (k == wlen && (cmd[i + k] == '\0' || cmd[i + k] == ' '))
            return 1;
    }
    return 0;
}

/* A quick heap test: allocate, write, free, reuse */
static int kmalloc_selftest(void)
{
    char *a = kmalloc(64);
    char *b = kmalloc(128);
    if (!a || !b)
        return 0;
    memset(a, 0xAA, 64);
    memset(b, 0x55, 128);
    if ((uint8_t)a[63] != 0xAA || (uint8_t)b[0] != 0x55)
        return 0;
    kfree(a);
    char *c = kmalloc(32);   /* it should land in the freed hole */
    int reused = (c == a);
    kfree(b);
    kfree(c);
    return reused;
}

/* Demonstration threads: A and B count at their own pace, C says
   hello, lives for 2 seconds and finishes (a test of task_exit plus
   cleanup). They draw in row 0 (like uptime) - the console cursor
   never stands there, so they do not overwrite the prompt. */

static void draw_labeled_counter(int x, const char *label, uint32_t n)
{
    char buf[32];
    int i = 0;
    while (label[i]) {
        buf[i] = label[i];
        i++;
    }
    char digits[11];
    int d = 0;
    do {
        digits[d++] = '0' + n % 10;
        n /= 10;
    } while (n);
    while (d)
        buf[i++] = digits[--d];
    buf[i] = '\0';
    vga_write_at(x, 0, buf);
}

static void demo_counter(void *arg)
{
    int slot = (int)arg;
    uint32_t n = 0;
    for (;;) {
        draw_labeled_counter(slot ? 33 : 18,
                             slot ? "watek-B: " : "watek-A: ", n++);
        task_sleep(slot ? 330 : 200);
    }
}

static void demo_mortal(void *arg)
{
    (void)arg;
    vga_write_at(48, 0, "watek-C: czesc!     ");
    task_sleep(2000);
    /* We keep redrawing the text for a while. Row 0 disappears on every
       scroll of the screen, and startup messages keep coming for a good
       second after the thread finishes its work - otherwise the proof
       that it ran at all would scroll away with them. */
    for (int i = 0; i < 12; i++) {
        vga_write_at(48, 0, "watek-C: zakonczony ");
        task_sleep(500);
    }

    /* The text in row zero disappears on every scroll of the screen, so
       on a loaded machine the proof can scroll away together with the
       startup messages before anybody reads it. So we also write it
       where nothing ever scrolls away.

       Straight to the serial port rather than through printk: the
       thread finishes after the shell has started and console output
       would land in the middle of a line somebody is typing. The
       serial port is the log, the screen is the work in progress; the
       two are not always the same. */
    serial_write("[ OK ] Watek demonstracyjny C zakonczyl prace\n");
}

static void ok(const char *what)
{
    vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
    printk("[ OK ] ");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    printk("%s\n", what);
}

void kmain(uint32_t magic, uint32_t multiboot_info_addr)
{
    /* The loader says in the magic which form it passed the information
       in; from here on the whole system sees it the same way */
    const struct multiboot_info *mbi = mb2_info(magic, multiboot_info_addr);

    /* When the loader set a graphics mode, the console has to draw on
       it - and before the first message is printed. Paging is not
       running yet, so the buffer is reachable directly; gluing it onto
       the mapping comes right after paging_init. */
    const struct mb2_framebuffer *lfb = mb2_framebuffer();
    if (lfb && video_adopt_lfb(lfb->addr, lfb->pitch, (int)lfb->width,
                               (int)lfb->height, (int)lfb->bpp) == 0)
        console_use_framebuffer();

    vga_init();
    serial_init();

    vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
    printk("ProtonOS\n");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    printk("Jadro uruchomione w trybie chronionym 32-bit.\n\n");

    if (!mbi) {
        vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
        printk("BLAD: nieznany magic od bootloadera (%x)!\n", magic);
        for (;;)
            __asm__ volatile("cli; hlt");
    }
    printk("[ OK ] Multiboot OK (%s)\n", mb2_boot_kind());

    gdt_init();
    ok("GDT zaladowana (plaski model pamieci, segmenty ring 0/3)");

    idt_init();
    ok("IDT zaladowana (wyjatki 0-31, IRQ 32-47)");

    pic_init();
    ok("PIC przemapowany (IRQ -> wektory 32-47)");

    pit_init(100);
    ok("Timer PIT: 100 Hz");

    keyboard_init();
    ok("Klawiatura PS/2 gotowa");

    /* A laptop whose touchpad is not on this port answers nothing
       here, and that is worth saying out loud rather than reporting a
       mouse that is not there. */
    if (mouse_init())
        printk("[ OK ] Mysz PS/2 na porcie pomocniczym (IRQ12), kolko: %s\n",
               mouse_has_wheel() ? "tak" : "nie");
    else
        printk("[ -- ] Na porcie pomocniczym PS/2 nie ma myszy "
               "(touchpad moze byc na USB albo I2C)\n");

    __asm__ volatile("sti");
    ok("Przerwania wlaczone (sti)");

    /* An IDT test: int3 should print a message and come back */
    __asm__ volatile("int $3");

    pmm_init(mbi);
    printk("[ OK ] Pamiec fizyczna: %u KiB RAM, %u wolnych ramek 4 KiB\n",
           pmm_ram_kib(), pmm_total_frames() - pmm_used_frames());

    paging_init();
    /* From now on every change of a mapping can be told to the other
       cores - and to do that they have to know what each is running */
    tlb_switch(paging_kernel_cr3());
    video_map_lfb();          /* before we write anything on the screen */
    printk("[ OK ] Stronicowanie wlaczone (identycznosciowe %u MiB)\n",
           paging_identity_limit() / (1024 * 1024));

    heap_init();
    if (kmalloc_selftest())
        printk("[ OK ] Sterta jadra: kmalloc/kfree dzialaja (%u KiB)\n",
               heap_total_bytes() / 1024);
    else
        printk("[FAIL] kmalloc selftest!\n");

    initrd_init(mbi);
    if (initrd_file_count())
        printk("[ OK ] initrd: %u plikow (komendy 'ls' i 'run')\n",
               initrd_file_count());

    /* The shared library has to be ready before the first program
       starts - its frames are shared by all processes */
    if (dyn_init())
        printk("[ OK ] Biblioteka %s: %u stron wspolnych dla procesow\n",
               dyn_name(), dyn_pages());

    /* Under UEFI there is no BIOS memory to search - the pointer to
       the ACPI tables then comes from the loader and nowhere else */
    if (mb2_rsdp())
        acpi_set_rsdp(mb2_rsdp());
    if (acpi_init())
        printk("[ OK ] ACPI: tablice od '%s' (komendy 'poweroff', 'reboot')\n",
               acpi_oem());
    else
        printk("[ !! ] ACPI: brak tablic - komputera nie wylaczymy\n");

    blk_init();
    if (fs_init())
        printk("[ OK ] Dysk %s + PFS gotowy (komendy 'fls', 'cat', 'write')\n",
               blk_name());

    /* What the machine remembers about itself. Right after the file
       system, because that is where it is written - and before the
       network, because one of the settings is for the network. */
    config_load();
    printk("[ OK ] Ustawienia: %s (%d pozycji, komenda 'set')\n",
           config_get("nazwa"), config_count());

    int has_net = net_init();
    if (has_net) {
        /* A name server from the settings takes precedence over the one
           DHCP hands out: it was written down by somebody on purpose,
           and that outranks what the network volunteers. */
        uint32_t dns = config_get_num("dns", 0);
        if (dns)
            net_set_dns(dns);
        const uint8_t *m = netdev_mac();
        printk("[ OK ] Siec: %s %x:%x:%x:%x:%x:%x "
               "(komendy 'ifconfig', 'ping', 'dhcp')\n", netdev_name(),
               m[0], m[1], m[2], m[3], m[4], m[5]);
    }

    /* The remaining processors: the firmware lists them in an ACPI
       table and each waits for an INIT-SIPI signal. Woken ones enter
       protected mode and stop - giving them work needs a per-core
       scheduler and locks throughout the kernel, and those were
       written for a single core. */
    int ncores = smp_init();
    if (ncores > 1)
        printk("[ OK ] Procesory: %d (obudzonych %d, czekaja na prace)\n",
               ncores, smp_started());
    else
        printk("[ OK ] Procesory: 1\n");

    scheduler_init();
    task_create("demo-A", demo_counter, (void *)0);
    task_create("demo-B", demo_counter, (void *)1);
    task_create("demo-C", demo_mortal, (void *)0);
    ok("Scheduler: watki jadra, wywlaszczanie co 10 ms");

    /* Swapping only here: memory reclaim walks the task list, so the
       scheduler has to exist already */
    swap_init();

    /* A USB keyboard needs the scheduler too - a thread polls it. On a
       machine without PS/2 it is the only input the system has. */
    usbkbd_init();
    usbmouse_init();
    /* And the adapter on the same bus. It reports in by itself when it
       finds one; on a machine without Bluetooth this costs one walk
       across the ports and says nothing. */
    bt_init();

    /* Sound. Not needed by anything and that is exactly why it is
       started here rather than on demand: a machine that cannot make a
       noise should say so at startup, not the first time somebody
       asks for one. */
    if (hda_init())
        printk("[ OK ] Dzwiek: Intel HDA, %s\n", hda_name());
    else
        printk("[ -- ] Dzwiek: %s\n", hda_name());

    /* We take the IP address from the network - the DHCP client waits
       for the server's answers, so it has to run in a thread (after
       scheduler_init) */
    if (has_net) {
        dhcp_start();
        tcp_init();        /* the TCP retransmission thread */
    }

    /* By design the system boots straight into the desktop; 'nodesktop'
       on the kernel command line leaves the text console (the smoke
       test and commands that print a lot of text use it). The message
       goes before shell_init, so that the last thing on the screen is
       the prompt. */
    int text_mode = cmdline_has(mbi, "nodesktop") || cmdline_has(mbi, "text");
    if (text_mode)
        printk("Tryb tekstowy (linia polecen jadra) - pulpit: 'desktop'\n");
    else
        printk("Start pulpitu graficznego (wylaczysz opcja 'nodesktop')\n");

    shell_init();

    if (!text_mode)
        desktop_start();

    /* The main thread becomes the idle thread - and such a thread may
       not hold the big kernel lock: a core standing on hlt with the
       lock in its hand would close the kernel for everybody else. */
    biglock_leave();
    for (;;)
        __asm__ volatile("hlt");
}
