/* ThreadOS - the kernel's main entry point in C */

#include "include/fonts.h"
#include <stdint.h>
#include "include/bootsplash.h"
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
#include "include/user.h"
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
#include "include/usbhc.h"
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
   boot/grub.cfg. */
static int cmdline_has(const struct multiboot_info *mbi, const char *word)
{
    if (!mbi || !(mbi->flags & MULTIBOOT_FLAG_CMDLINE) || !mbi->cmdline)
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
                             slot ? "thread-B: " : "thread-A: ", n++);
        task_sleep(slot ? 330 : 200);
    }
}

static void demo_mortal(void *arg)
{
    (void)arg;
    vga_write_at(48, 0, "thread-C: hello!    ");
    task_sleep(2000);
    /* We keep redrawing the text for a while. Row 0 disappears on every
       scroll of the screen, and startup messages keep coming for a good
       second after the thread finishes its work - otherwise the proof
       that it ran at all would scroll away with them. */
    for (int i = 0; i < 12; i++) {
        vga_write_at(48, 0, "thread-C: finished  ");
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
    serial_write("[ OK ] The demonstration thread C has finished\n");
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
    int text_mode = !mbi || cmdline_has(mbi, "nodesktop") ||
                    cmdline_has(mbi, "text");

    /* When the loader set a graphics mode, the console has to draw on
       it - and before the first message is printed. Paging is not
       running yet, so the buffer is reachable directly; gluing it onto
       the mapping comes right after paging_init. */
    const struct mb2_framebuffer *lfb = mb2_framebuffer();
    if (lfb && video_adopt_lfb(lfb->addr, lfb->pitch, (int)lfb->width,
                               (int)lfb->height, (int)lfb->bpp) == 0) {
        video_panel_mm((int)lfb->mm_w, (int)lfb->mm_h);
        console_use_framebuffer();
    }

    vga_init();
    serial_init();
    boot_splash_start(!text_mode);

    vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
    printk("ThreadOS\n");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    printk("The kernel is running in 32-bit protected mode.\n\n");

    if (!mbi) {
        printk_screen_output(1);
        vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
        printk("ERROR: unknown magic from the boot loader (%x)!\n", magic);
        for (;;)
            __asm__ volatile("cli; hlt");
    }
    printk("[ OK ] Multiboot OK (%s)\n", mb2_boot_kind());

    gdt_init();
    ok("GDT loaded (flat memory model, ring 0/3 segments)");

    idt_init();
    ok("IDT loaded (exceptions 0-31, IRQ 32-47)");

    pic_init();
    ok("PIC remapped (IRQ -> vectors 32-47)");

    pit_init(100);
    ok("Timer PIT: 100 Hz");

    keyboard_init();
    ok("The PS/2 keyboard is ready");

    /* A laptop whose touchpad is not on this port answers nothing
       here, and that is worth saying out loud rather than reporting a
       mouse that is not there. */
    if (mouse_init())
        printk("[ OK ] PS/2 mouse on the auxiliary port (IRQ12), wheel: %s\n",
               mouse_has_wheel() ? "yes" : "no");
    else
        printk("[ -- ] No mouse on the auxiliary PS/2 port "
               "(the touchpad may be on USB or I2C)\n");

    __asm__ volatile("sti");
    ok("Interrupts enabled (sti)");

    /* An IDT test: int3 should print a message and come back */
    __asm__ volatile("int $3");

    pmm_init(mbi);
    printk("[ OK ] Physical memory: %u KiB RAM, %u free 4 KiB frames\n",
           pmm_ram_kib(), pmm_total_frames() - pmm_used_frames());

    paging_init();
    /* From now on every change of a mapping can be told to the other
       cores - and to do that they have to know what each is running */
    tlb_switch(paging_kernel_cr3());
    video_map_lfb();          /* before we write anything on the screen */
    boot_splash_update(18, "Preparing memory");
    printk("[ OK ] Paging enabled (identity-mapped %u MiB)\n",
           paging_identity_limit() / (1024 * 1024));

    heap_init();
    if (kmalloc_selftest())
        printk("[ OK ] Kernel heap: kmalloc/kfree work (%u KiB)\n",
               heap_total_bytes() / 1024);
    else
        printk("[FAIL] kmalloc selftest!\n");

    initrd_init(mbi);
    fonts_init();
    if (initrd_file_count())
        printk("[ OK ] initrd: %u files (the 'ls' and 'run' commands)\n",
               initrd_file_count());
    boot_splash_update(36, "Loading system files");

    /* The shared library has to be ready before the first program
       starts - its frames are shared by all processes */
    if (dyn_init())
        printk("[ OK ] Library %s: %u pages shared between processes\n",
               dyn_name(), dyn_pages());

    /* Under UEFI there is no BIOS memory to search - the pointer to
       the ACPI tables then comes from the loader and nowhere else */
    if (mb2_rsdp())
        acpi_set_rsdp(mb2_rsdp());
    if (acpi_init())
        printk("[ OK ] ACPI: tables from '%s' (the 'poweroff', 'reboot' commands)\n",
               acpi_oem());
    else
        printk("[ !! ] ACPI: no tables - we cannot switch the machine off\n");

    blk_init();
    if (fs_init())
        printk("[ OK ] Disk %s + TFS ready (the 'fls', 'cat', 'write' commands)\n",
               blk_name());
    boot_splash_update(60, "Checking storage");

    /* What the machine remembers about itself. Right after the file
       system, because that is where it is written - and before the
       network, because one of the settings is for the network. */
    config_load();
    user_accounts_load();
    printk("[ OK ] Settings: %s (%d entries, the 'set' command)\n",
           config_get("name"), config_count());

    int has_net = net_init();
    if (has_net) {
        /* A name server from the settings takes precedence over the one
           DHCP hands out: it was written down by somebody on purpose,
           and that outranks what the network volunteers. */
        uint32_t dns = config_get_num("dns", 0);
        if (dns)
            net_set_dns(dns);
        const uint8_t *m = netdev_mac();
        printk("[ OK ] Network: %s %x:%x:%x:%x:%x:%x "
               "(the 'ifconfig', 'ping', 'dhcp' commands)\n", netdev_name(),
               m[0], m[1], m[2], m[3], m[4], m[5]);
    }

    /* The remaining processors: the firmware lists them in an ACPI
       table and each waits for an INIT-SIPI signal. Woken ones enter
       protected mode and stop - giving them work needs a per-core
       scheduler and locks throughout the kernel, and those were
       written for a single core. */
    int ncores = smp_init();
    if (ncores > 1)
        printk("[ OK ] Processors: %d (woken %d, waiting for work)\n",
               ncores, smp_started());
    else
        printk("[ OK ] Processors: 1\n");

    scheduler_init();
    task_create("demo-A", demo_counter, (void *)0);
    task_create("demo-B", demo_counter, (void *)1);
    task_create("demo-C", demo_mortal, (void *)0);
    ok("Scheduler: kernel threads, preemption every 10 ms");

    /* Swapping only here: memory reclaim walks the task list, so the
       scheduler has to exist already */
    swap_init();

    /* A USB keyboard needs the scheduler too - a thread polls it. On a
       machine without PS/2 it is the only input the system has. */
    usbkbd_init();
    usbmouse_init();
    /* The sockets themselves, watched for a stick pushed in later. The
       disk driver asked for this already, while the disks were being
       found - but that was before there was a scheduler, and a thread
       needs one. */
    usbhc_watch();
    /* And the adapter on the same bus. It reports in by itself when it
       finds one; on a machine without Bluetooth this costs one walk
       across the ports and says nothing. */
    bt_init();

    /* Sound. Not needed by anything and that is exactly why it is
       started here rather than on demand: a machine that cannot make a
       noise should say so at startup, not the first time somebody
       asks for one. */
    if (hda_init())
        printk("[ OK ] Sound: Intel HDA, %s\n", hda_name());
    else
        printk("[ -- ] Sound: %s\n", hda_name());
    boot_splash_update(84, "Starting devices");

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
    if (text_mode)
        printk("Text mode (the kernel's command line) - desktop: 'desktop'\n");
    else
        printk("Starting the graphical desktop ('nodesktop' switches it off)\n");

    shell_init();

    if (!text_mode) {
        boot_splash_update(96, "Starting desktop");
        desktop_start();
    }

    /* The main thread becomes the idle thread - and such a thread may
       not hold the big kernel lock: a core standing on hlt with the
       lock in its hand would close the kernel for everybody else. */
    biglock_leave();
    for (;;)
        __asm__ volatile("hlt");
}
