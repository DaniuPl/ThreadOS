/* The 64-bit kernel's shell
 *
 * An ordinary kernel thread: it reads characters from the keyboard,
 * assembles lines out of them and runs commands. There are no
 * interrupts in it and no global state beyond its own line buffer -
 * it is a plain loop that happens to run in parallel with the rest of
 * the system.
 *
 * There are as many commands as the kernel really knows how to do.
 * Each of them shows something that cannot be seen any other way than
 * by asking the system while it runs - and that is the whole
 * difference between a shell and a script printed out at startup.
 */

#include "../kernel/include/fonts.h"
#include "../kernel/include/gfx.h"
#include <stdint.h>
#include "shell64.h"
#include "keyboard64.h"
#include "../kernel/include/user.h"
#include "../kernel/include/clip.h"
#include "../kernel/include/keyboard.h"
#include "task64.h"
#include "paging64.h"
#include "elf64.h"
#include "proc64.h"
#include "../kernel/include/prog.h"
#include "isr64.h"
#include "task64.h"
#include "syscall64.h"
#include "../kernel/include/printk.h"
#include "../kernel/include/string.h"
#include "../kernel/include/pmm.h"
#include "../kernel/include/pit.h"
#include "../kernel/include/initrd.h"
#include "../kernel/include/blk.h"
#include "../kernel/include/part.h"
#include "../kernel/include/acpi.h"
#include "../kernel/include/swap.h"
#include "../kernel/include/rtc.h"
#include "../kernel/include/smp.h"
#include "../kernel/include/tlb.h"
#include "../kernel/include/isr.h"
#include "../kernel/include/config.h"
#include "../kernel/include/vga.h"
#include "../kernel/include/smpmem.h"
#include "../kernel/include/smpthreads.h"
#include "../kernel/include/io.h"
#include "../kernel/include/fs.h"
#include "../kernel/include/fatcmd.h"
#include "../kernel/include/install.h"
#include "../kernel/include/hwinfo.h"
#include "../kernel/include/hda.h"
#include "../kernel/include/bt.h"
#include "../kernel/include/fbcon.h"
#include "../kernel/include/net.h"
#include "../kernel/include/video.h"
#include "../kernel/include/desktop.h"
#include "../kernel/include/vga.h"

#define LINE_MAX 80

/* Does the line start with the given word? (the kernel has no strncmp
   - and has no reason to have one for a single place) */
static int starts_with(const char *s, const char *prefix)
{
    while (*prefix)
        if (*s++ != *prefix++)
            return 0;
    return 1;
}


/* The system's own files: reading them is nobody's business, writing
   to them is changing the system - so the console asks the same
   question the syscalls ask. */
static int may_touch(const char *name)
{
    if (!user_is_system_file(name) || user_shell_level() == USER_ADMIN)
        return 1;
    printk("no permission for '%s' - try it through sudo\n", name);
    return 0;
}

static void prompt(void)
{
    /* The prompt says the machine's name and wears the colour it was
       given - both come from the settings, so both survive a restart */
    vga_set_color((enum vga_color)(config_get_num("color", VGA_LIGHT_GREEN) & 15),
                  VGA_BLACK);
    /* '#' for an admin, '>' for everybody else - the oldest way of
       saying it, and the one that is seen without being read */
    printk("%s64%c ", config_get("name"),
           user_shell_level() == USER_ADMIN ? '#' : '>');
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
}

static void cmd_help(void)
{
    printk("Commands: help, ps, mem, set, ls, disk, fls, pwd, cd <dir>,\n");
    printk("          mkdir <dir>, cat <file>, write <file> <text>,\n");
    printk("          rm <file>, cp <source> <target>, uptime, date,\n");
    printk("          run <program>, clear,\n");
    printk("          ifconfig, arp, ping <address>, dns <ip>,\n");
    printk("          gfx, desktop,\n");
    printk("          reboot, poweroff, smp, smpmem, smpthreads\n");
    printk("          runw <prog> - a program on any core\n");
    printk("          whoami, sudo <command>, logout, passwd\n");
    printk("          fat [N [disk]] - the FAT partition: fatls, fatcat,\n");
    printk("            fatsum, fatput, fatcopy, fatmkdir, fatrm\n");
    printk("          hardware - what is in this machine, disks, font\n");
    printk("          bt [scan|test] - Bluetooth\n");
    printk("          beep [Hz] [ms] - sound, volume [0-100|mute]\n");
    printk("          install - ThreadOS onto this machine's disk\n");
    printk("            (the whole screen; 'install text [whole]' - here)\n");
}

static void cmd_mem(void)
{
    uint32_t total = pmm_total_frames();
    uint32_t used = pmm_used_frames();
    printk("Memory: %u KiB RAM, frames %u/%u used (%u KiB free)\n",
           pmm_ram_kib(), used, total, (total - used) * 4);
    printk("Paging: identity-mapped %u MiB, split pages %u\n",
           (uint32_t)(paging64_identity_top() / (1024 * 1024)),
           paging64_cow_splits());

    uint32_t wuz, not_at_all, wout, win;
    swap_stats(&wuz, &not_at_all, &wout, &win);
    if (not_at_all)
        printk("Swap: %u/%u pages on disk "
               "(evicted %u, fetched %u)\n", wuz, not_at_all, wout, win);
    else
        printk("Swap: no swap file\n");
}

/* Writing and reading a sector - the only way to check the disk for
   real rather than merely that the controller introduced itself.
 *
 * The last sector, and that used to be safe because our disks had
 * nothing on them. It is not safe on a disk with a GPT: the last
 * sector of such a disk IS the copy of the partition table, and a
 * write there destroys it without a word - the disk still boots, and
 * one day a repair tool says the table is damaged. So a disk with a
 * table on it is read and not written, and this says so. */
static void cmd_disk(void)
{
    if (!blk_present()) {
        printk("disk: there is no disk\n");
        return;
    }
    printk("Disk: %s, %u sectors (%u MiB)\n",
           blk_name(), blk_sectors(), blk_sectors() / 2048);

    static uint8_t buf[512];
    uint32_t lba = blk_sectors() ? blk_sectors() - 1 : 0;

    if (part_scan() > 0) {
        if (blk_read(lba, 1, buf) < 0) {
            printk("disk: reading sector %u did not work\n", lba);
            return;
        }
        printk("disk: the disk has a partition table - the last sector is "
               "its copy,\n      so I only read it (%u B read)\n",
               512u);
        return;
    }

    for (int i = 0; i < 512; i++)
        buf[i] = (uint8_t)(i + 7);
    if (blk_write(lba, 1, buf) < 0) {
        printk("disk: writing sector %u did not work\n", lba);
        return;
    }

    for (int i = 0; i < 512; i++)
        buf[i] = 0;
    if (blk_read(lba, 1, buf) < 0) {
        printk("disk: reading sector %u did not work\n", lba);
        return;
    }

    int differ = 0;
    for (int i = 0; i < 512; i++)
        if (buf[i] != (uint8_t)(i + 7))
            differ++;
    printk("disk: sector %u written and read back, differences %d\n", lba, differ);
}

/* --- File system --------------------------------------------------
 *
 * Three commands are enough to check whether a file really lies on the
 * disk and not only in memory: write it, print it, see it in the
 * listing. The fourth proof arrives by itself - after a restart.
 */
static void cmd_pwd(void)
{
    char path[FS_NAME_MAX * 4];
    fs_getpath(task64_cwd(), path);
    printk("%s\n", path);
}

static void cmd_fls(void)
{
    if (!fs_ready()) {
        printk("fls: there is no filesystem\n");
        return;
    }

    /* We list the directory the shell is standing in - not always the
   root one */
    char name[FS_NAME_MAX + 1];
    uint32_t size = 0;
    int dir = 0;
    cmd_pwd();
    for (int i = 0; fs_readdir(task64_cwd(), i, name,
                               &size, &dir) == 1; i++)
        printk("  %s%s  (%u B)\n", name, dir ? "/" : "", size);

    uint32_t used = 0, total = 0;
    fs_usage(&used, &total);
    printk("Used: %u/%u blocks of %u B\n", used, total, FS_BLOCK);
}

static void cmd_cat(const char *name)
{
    static char buf[512];

    if (!fs_ready()) {
        printk("cat: there is no filesystem\n");
        return;
    }
    int slot = fs_open(task64_cwd(), name, 0);
    int n = slot < 0 ? -1 : fs_read_at(slot, 0, buf, sizeof(buf) - 1);
    if (n < 0) {
        printk("cat: there is no file '%s'\n", name);
        return;
    }
    buf[n] = '\0';
    printk("%s", buf);
    if (n && buf[n - 1] != '\n')
        printk("\n");
}

/* write <file> <text> - the rest of the line goes into the file */
static void cmd_write(char *rest)
{
    if (!fs_ready()) {
        printk("write: there is no filesystem\n");
        return;
    }

    char *body = rest;
    while (*body && *body != ' ')
        body++;
    if (!*body) {
        printk("write: give a name and the text\n");
        return;
    }
    *body++ = '\0';                 /* the name ends here */
    while (*body == ' ')
        body++;

    uint32_t len = 0;
    while (body[len])
        len++;
    body[len++] = '\n';             /* a text file ends with a newline */

    if (!may_touch(rest))
        return;

    int slot = fs_open(task64_cwd(), rest, 1);
    if (slot < 0 || fs_truncate(slot) < 0 ||
        fs_write_at(slot, 0, body, len) < 0)
        printk("write: could not write '%s'\n", rest);
    else
        printk("write: '%s' written (%u B)\n", rest, len);
}

/* smp - spreading the counting across all cores
 *
 * Every work item computes a checksum over a piece of a pseudorandom
 * sequence - enough work for a core to have something to do, and
 * simple enough that the result can be verified.
 *
 * We compare the result with one computed on a single core. It matches
 * when every item was computed exactly once and nobody trampled
 * somebody else's cell - and that is the right question to ask about
 * concurrency, not whether anything got computed at all.
 */

#define SMP_UNITS 64
#define SMP_STEPS    200000

static uint32_t smp_results[SMP_UNITS];

/* A linear congruential generator - always the same sequence for a
   given number, so the result does not depend on which core computed
   it */
static uint32_t smp_run_chunk(uint32_t number)
{
    uint32_t stan = number * 2654435761u + 12345u;
    uint32_t sum = 0;
    for (uint32_t i = 0; i < SMP_STEPS; i++) {
        stan = stan * 1103515245u + 12345u;
        sum += stan >> 16;
    }
    return sum;
}

static void smp_unit(uint32_t number, void *data)
{
    (void)data;
    if (number < SMP_UNITS)
        smp_results[number] = smp_run_chunk(number);
}

static void cmd_smp(void)
{
    uint32_t pattern = 0;
    for (uint32_t i = 0; i < SMP_UNITS; i++)
        pattern += smp_run_chunk(i);

    for (uint32_t i = 0; i < SMP_UNITS; i++)
        smp_results[i] = 0;
    smp_work(smp_unit, 0, SMP_UNITS);

    uint32_t total = 0;
    for (uint32_t i = 0; i < SMP_UNITS; i++)
        total += smp_results[i];

    printk("Cores: %d (woken %d), units %u\n",
           smp_cpus(), smp_started(), (uint32_t)SMP_UNITS);
    printk("Share:");
    for (int i = 0; i < smp_cpus(); i++)
        printk(" core%d=%u", i, smp_share(i));
    printk("\n");
    printk("Result: %u, expected: %u - %s\n", total, pattern,
           total == pattern ? "matches" : "DOES NOT MATCH");
    printk("Cores that counted: %d\n", smp_workers());

    /* Every core's own clock. The bootstrap core runs off the PIT and
       has zero here - the secondary ones count their own ticks in the
       local APIC, and that is the only proof that they receive
       interrupts at all. */
    printk("Core clocks:");
    for (int i = 0; i < smp_cpus(); i++)
        printk(" core%d=%u", i, smp_ticks(i));
    printk("\n");

    /* What the changed mappings cost. 'lokalnie' are the ones nobody
       else was holding - no interrupt, no waiting; 'zdalnie' are the
       ones that had to reach another core, and 'odpowiedzi' is how many
       cores answered. */
    printk("TLB: local %u, remote %u, replies %u\n",
           tlb_local_only(), tlb_shootdowns(), tlb_remote_acks());
    /* The commonest way into the kernel there is. 'bez zamka' are the
       faults served before the big kernel lock was taken at all. */
    printk("Page faults: without the lock %u, with it %u\n",
           isr_faults_nolock(), isr_faults_locked());
}

/* cp <source> <target> - copying a file onto the disk
 *
 * We look for the source where 'run' looks for it: first in the
 * initrd, then on the disk. Thanks to that a program shipped together
 * with the kernel can be moved onto the disk with a single command -
 * and run from there from then on, including when it is no longer in
 * the initrd. */
static void cmd_cp(char *rest)
{
    if (!fs_ready()) {
        printk("cp: there is no filesystem\n");
        return;
    }

    char *dst = rest;
    while (*dst && *dst != ' ')
        dst++;
    if (!*dst) {
        printk("cp: give a source and a target, e.g. 'cp hello64 hello64'\n");
        return;
    }
    *dst++ = '\0';                    /* the source name ends here */
    while (*dst == ' ')
        dst++;
    if (!*dst) {
        printk("cp: give a target\n");
        return;
    }

    if (!may_touch(dst))
        return;

    uint32_t size = 0;
    void *loaded = (void *)0;
    const void *image = prog_image(rest, &size, &loaded);
    if (!image) {
        printk("cp: there is no '%s'\n", rest);
        return;
    }

    /* fs_write_at returns the NUMBER of bytes written, not zero - a
       file cut off halfway has to count as an error too */
    int slot = fs_open(task64_cwd(), dst, 1);
    int ok = slot >= 0 && fs_truncate(slot) >= 0 &&
             fs_write_at(slot, 0, image, size) == (int)size;
    prog_release(loaded);

    if (ok)
        printk("cp: '%s' -> '%s' (%u B)\n", rest, dst, size);
    else
        printk("cp: could not write '%s'\n", dst);
}

/* A drawing that checks graphics mode.
 *
 * Four rectangles in the corners, a frame and a caption - that is
 * enough to see whether the colours land where they should and
 * whether the pixel at (x, y) ends up at (x, y). A wrongly computed
 * bytes-per-pixel shows up here immediately: the image skews.
 */
/* 'bt' - what the adapter is, 'bt skan [s]' - who is in range,
   'bt test' - the packet parser against recorded packets, which is the
   only part of Bluetooth that can be checked without an adapter. */
static void cmd_bt(const char *arg)
{
    while (*arg == ' ')
        arg++;

    if (arg[0] == 't') {                 /* test */
        bt_selftest();
        return;
    }

    if (!bt_present()) {
        printk("bt: there is no adapter (%s)\n", bt_why());
        return;
    }

    if (arg[0] != 's') {                 /* status */
        printk("adapter: %s\n", bt_address());
        printk("name:    %s\n", bt_name());
        printk("version: %s\n", bt_version());
        printk("( 'bt scan' - who is within reach )\n");
        return;
    }

    uint32_t sec = 0;
    for (const char *p = arg; *p; p++)
        if (*p >= '0' && *p <= '9')
            sec = sec * 10 + (uint32_t)(*p - '0');
    if (!sec || sec > 30)
        sec = 5;

    printk("bt: listening for %u s...\n", sec);
    int n = bt_scan(sec);
    if (n < 0) {
        printk("bt: the adapter would not start listening\n");
        return;
    }
    for (int i = 0; i < n; i++)
        printk("  %s  %s%s%d dBm\n", bt_found_addr(i), bt_found_name(i),
               bt_found_name(i)[0] ? "  " : "", bt_found_rssi(i));
    printk("bt: devices heard: %u\n", (uint32_t)n);
}

static void cmd_gfx(void)
{
    if (!video_w()) {
        printk("gfx: there is no graphics mode\n");
        return;
    }
    int w = video_w(), h = video_h();

    /* The message goes BEFORE the drawing: the console and the drawing
       share the same buffer and every print redraws it in full - so
       anything written after the drawing lands on top of it. */
    printk("gfx: drawing %ux%u, %u bits per pixel\n",
           (uint32_t)w, (uint32_t)h, (uint32_t)fbcon_bpp());

    /* video.c's palette defines indices 0-5, not the sixteen colours of
       text mode: 1 red, 2 green, 3 blue, 4 white, 5 yellow. */
    video_clear(3);                                    /* blue background */
    video_fill_rect(0, 0, w / 4, h / 4, 1);            /* top left     */
    video_fill_rect(w - w / 4, 0, w / 4, h / 4, 2);    /* top right    */
    video_fill_rect(0, h - h / 4, w / 4, h / 4, 5);    /* bottom left  */
    video_fill_rect(w - w / 4, h - h / 4, w / 4, h / 4, 4);

    for (int x = 0; x < w; x++) {
        video_putpixel(x, 0, 4);
        video_putpixel(x, h - 1, 4);
    }
    for (int y = 0; y < h; y++) {
        video_putpixel(0, y, 4);
        video_putpixel(w - 1, y, 4);
    }
    video_draw_string(w / 2 - 112, h / 2, "ThreadOS 64 - graphics mode", 4);
    video_present();
}

/* date - the time from the motherboard clock */
/* set - what the machine remembers about itself
 *
 * Without arguments it shows everything, with a name and a value it
 * changes one and writes them all back to the disk. The proof that it
 * worked is not the line it prints but the next boot. */
static void cmd_set(const char *arg)
{
    while (*arg == ' ')
        arg++;
    if (!*arg) {
        printk("Settings (file %s):\n", CONFIG_FILE);
        for (int i = 0; i < config_count(); i++)
            printk("  %s = %s   (%s)\n", config_name(i), config_value(i),
                   config_help(i));
        printk("usage: set <name> <value>\n");
        return;
    }

    char name[32];
    int i = 0;
    while (arg[i] && arg[i] != ' ' && i < (int)sizeof(name) - 1) {
        name[i] = arg[i];
        i++;
    }
    name[i] = 0;
    while (arg[i] == ' ')
        i++;
    if (!arg[i]) {
        const char *v = config_get(name);
        if (v)
            printk("%s = %s\n", name, v);
        else
            printk("set: there is no setting '%s'\n", name);
        return;
    }

    if (user_shell_level() != USER_ADMIN) {
        printk("set: changing settings needs admin - 'sudo set %s ...'\n",
               name);
        return;
    }
    if (config_is_hidden(name)) {
        printk("set: '%s' does not change this way - see the 'passwd' command\n",
               name);
        return;
    }
    if (config_set(name, arg + i) < 0) {
        printk("set: could not write '%s' (bad name, too long a "
               "value, or no disk)\n", name);
        return;
    }
    printk("%s = %s (saved)\n", name, config_get(name));
}

static void cmd_date(void)
{
    struct rtc_time t;
    rtc_read(&t);
    printk("%u-%u-%u %u:%u:%u\n", (uint32_t)t.year, (uint32_t)t.month,
           (uint32_t)t.day, (uint32_t)t.hour, (uint32_t)t.min,
           (uint32_t)t.sec);
}

static void cmd_uptime(void)
{
    uint32_t t = pit_ticks();
    printk("Uptime: %u s (%u ticks of 10 ms)\n", t / 100, t);
}

/* The command's argument: the first word after the name, with no
   space before it */
static const char *argument(const char *line, int cmd_len)
{
    int i = cmd_len;
    while (line[i] == ' ')
        i++;
    return line[i] ? &line[i] : (const char *)0;
}

/* --- who is at the machine ------------------------------------------
 *
 * The shell reads characters one at a time in its own thread, so a
 * password is simply a blocking read here, with stars in place of what
 * was typed. Nothing asynchronous, nothing kept afterwards.
 */
static void read_secret(char *out, int max)
{
    int n = 0;
    for (;;) {
        char c = keyboard64_getchar();
        if (c == '\n')
            break;
        if (c == '\b') {
            if (n) {
                n--;
                printk("\b");
            }
            continue;
        }
        if (c < 0x20)
            continue;
        if (n < max - 1) {
            out[n++] = c;
            printk("*");
        }
    }
    out[n] = 0;
    printk("\n");
}

/* The same loop with what was typed left visible. A confirmation has
   to be READ by the person typing it. */
static void read_word(char *out, int max)
{
    int n = 0;
    for (;;) {
        char c = keyboard64_getchar();
        if (c == '\n')
            break;
        if (c == '\b') {
            if (n) {
                n--;
                printk("\b");
            }
            continue;
        }
        if (c < 0x20)
            continue;
        if (n < max - 1) {
            out[n++] = c;
            printk("%c", c);
        }
    }
    out[n] = 0;
    printk("\n");
}

/* Installing onto the machine's own disk.
 *
 * Reading and printing first, then a person saying yes, and only then
 * writing. The word typed out is deliberate: a key to press is
 * something one does by reflex, and a word is something one has to
 * decide to type - and what is on the other side of this is somebody's
 * laptop. */
/* 'install' takes the whole screen; 'install text' says the same
   things here, which is what one wants when there is no screen to draw
   on - or when the answer has to end up in a log. */
/* A number written out on the line - the disk to install onto,
   counted from one the way a person counts. */
static uint32_t text_num(const char *s)
{
    uint32_t v = 0;
    while (*s == ' ')
        s++;
    while (*s >= '0' && *s <= '9')
        v = v * 10 + (uint32_t)(*s++ - '0');
    return v;
}

/* A tone, and the shortest way to hear whether the sound driver found
   anything at all. Without arguments: A above middle C for a fifth of
   a second, because a number one recognises is worth more than a
   number one has to look up. */
static void cmd_beep(const char *arg)
{
    uint32_t hz = 440, ms = 200;
    const char *p = arg;

    if (!hda_present()) {
        printk("beep: there is no sound (see 'hardware')\n");
        return;
    }
    if (p && *p) {
        hz = text_num(p);
        while (*p == ' ')
            p++;
        while (*p >= '0' && *p <= '9')
            p++;
        if (*p)
            ms = text_num(p);
    }
    if (hz < 20 || hz > 20000) {
        printk("beep: %u Hz is not a sound anybody can hear\n", hz);
        return;
    }
    printk("beep: %u Hz for %u ms\n", hz, ms);
    if (hda_beep(hz, ms) < 0)
        printk("beep: could not play it\n");
}

static void cmd_install_text(int mode, int disk)
{
    char answer[8];

    if (blk_count() > 1)
        for (int i = 0; i < blk_count(); i++)
            printk("  disk %u: %s, %u MiB%s\n", (uint32_t)(i + 1),
                   blk_name_of(i), blk_sectors_of(i) / 2048,
                   i == (disk < 0 ? blk_current() : disk) ? "  <-" : "");
    if (!install_plan(mode, disk))
        return;
    /* A different word for the way that destroys things. YES is what
       one says to a question; ERASE is what one says to this. */
    const char *word = mode == INST_WHOLE ? "ERASE" : "YES";
    printk("\nType %s (in capitals) to do it: ", word);
    read_word(answer, (int)sizeof(answer));
    if (strcmp(answer, word) != 0) {
        printk("install: nothing was written\n");
        return;
    }
    install_do();
}

static void execute(const char *line);

static void cmd_whoami(void)
{
    printk("%s\n", user_shell_level() == USER_ADMIN
                        ? "admin (you can change the system)"
                        : "user (change the system through 'sudo')");
}

static void cmd_sudo(const char *arg)
{
    if (user_shell_level() == USER_ADMIN) {
        if (*arg)
            execute(arg);
        else
            printk("you are admin already\n");
        return;
    }

    char pass[CONFIG_VAL_MAX];
    printk("password: ");
    read_secret(pass, sizeof(pass));
    if (!user_password_ok(pass)) {
        printk("sudo: wrong password\n");
        return;
    }

    user_shell_set(USER_ADMIN);
    if (!*arg) {
        printk("you are admin; 'logout' ends it\n");
        return;
    }
    execute(arg);
    user_shell_set(USER_PLAIN);        /* one command, not a session */
}

static void cmd_logout(void)
{
    if (user_shell_level() != USER_ADMIN) {
        printk("you are not admin\n");
        return;
    }
    user_shell_set(USER_PLAIN);
    printk("an ordinary user again\n");
}

static void cmd_password(void)
{
    char pass[CONFIG_VAL_MAX], again[CONFIG_VAL_MAX];

    if (user_shell_level() != USER_ADMIN) {
        printk("old password: ");
        read_secret(pass, sizeof(pass));
        if (!user_password_ok(pass)) {
            printk("passwd: wrong password\n");
            return;
        }
    }
    printk("new password: ");
    read_secret(pass, sizeof(pass));
    if (!pass[0]) {
        printk("passwd: an empty password is no password - not changing it\n");
        return;
    }
    printk("again: ");
    read_secret(again, sizeof(again));
    if (strcmp(pass, again)) {
        printk("passwd: those two are not the same - not changing it\n");
        return;
    }
    if (user_set_password(pass) < 0) {
        printk("passwd: could not save it (no disk?)\n");
        return;
    }
    printk("password changed\n");
}

static void execute(const char *line)
{
    if (!line[0])
        return;

    if (!strcmp(line, "whoami")) {
        cmd_whoami();
        return;
    }
    if (!strcmp(line, "sudo")) {
        cmd_sudo("");
        return;
    }
    if (starts_with(line, "sudo ")) {
        cmd_sudo(line + 5);
        return;
    }
    if (!strcmp(line, "logout")) {
        cmd_logout();
        return;
    }
    if (!strcmp(line, "passwd")) {
        cmd_password();
        return;
    }

    if (!strcmp(line, "help")) {
        cmd_help();
    } else if (!strcmp(line, "ps")) {
        task64_dump();
    } else if (!strcmp(line, "mem")) {
        cmd_mem();
    } else if (!strcmp(line, "ls")) {
        initrd_ls();
    } else if (!strcmp(line, "fls")) {
        cmd_fls();
    } else if (!strcmp(line, "pwd")) {
        cmd_pwd();
    } else if (starts_with(line, "cd ")) {
        const char *kat = argument(line, sizeof("cd ") - 1);
        int fresh_one = FS_ROOT;
        if (!kat)
            printk("cd: give a directory\n");
        else if (fs_chdir(task64_cwd(), kat, &fresh_one) < 0)
            printk("cd: there is no directory '%s'\n", kat);
        else
            task64_set_cwd(fresh_one);
    } else if (starts_with(line, "mkdir ")) {
        const char *kat = argument(line, sizeof("mkdir ") - 1);
        if (!kat)
            printk("mkdir: give a name\n");
        else if (fs_mkdir(task64_cwd(), kat) < 0)
            printk("mkdir: could not create '%s'\n", kat);
        else
            printk("mkdir: '%s' created\n", kat);
    } else if (starts_with(line, "rm ")) {
        const char *file = argument(line, sizeof("rm ") - 1);
        if (!file)
            printk("rm: give a name\n");
        else if (!may_touch(file))
            ;                        /* said so already */
        else if (fs_unlink(task64_cwd(), file) < 0)
            printk("rm: there is no '%s'\n", file);
        else
            printk("rm: '%s' removed\n", file);
    } else if (starts_with(line, "cat ")) {
        const char *name = argument(line, sizeof("cat ") - 1);
        if (name)
            cmd_cat(name);
        else
            printk("cat: give a file name\n");
    } else if (!strcmp(line, "volume")) {
        hwinfo_volume("");
    } else if (starts_with(line, "volume ")) {
        hwinfo_volume(argument(line, sizeof("volume ") - 1));
    } else if (!strcmp(line, "beep")) {
        cmd_beep("");
    } else if (starts_with(line, "beep ")) {
        cmd_beep(argument(line, sizeof("beep ") - 1));
    } else if (!strcmp(line, "hardware")) {
        hwinfo_print();
    } else if (!strcmp(line, "bt")) {
        cmd_bt("");
    } else if (starts_with(line, "bt ")) {
        cmd_bt(line + 3);
    } else if (!strcmp(line, "font")) {
        fonts_command("");
    } else if (starts_with(line, "font ")) {
        fonts_command(line + sizeof("font ") - 1);
    } else if (starts_with(line, "shape ")) {
        gfx_command(line + sizeof("shape ") - 1);
    } else if (!strcmp(line, "disks")) {
        hwinfo_disks();
    } else if (starts_with(line, "disks read")) {
        hwinfo_disk_read(line + sizeof("disks read") - 1);
    } else if (!strcmp(line, "install")) {
        if (!install_graphical()) {
            printk("install: the desktop is running, and the installer needs "
                   "the whole screen.\n");
            printk("         Restart the machine, or type "
                   "'install text'.\n");
        }
    } else if (!strcmp(line, "install text")) {
        cmd_install_text(INST_ALONGSIDE, -1);
    } else if (!strcmp(line, "install text whole")) {
        cmd_install_text(INST_WHOLE, -1);
    } else if (starts_with(line, "install text whole ")) {
        cmd_install_text(INST_WHOLE,
                         (int)text_num(line + sizeof("install text whole ") - 1) - 1);
    } else if (starts_with(line, "install text ")) {
        cmd_install_text(INST_ALONGSIDE,
                         (int)text_num(line + sizeof("install text ") - 1) - 1);
    } else if (!strcmp(line, "fat")) {
        fatcmd_mount("");
    } else if (starts_with(line, "fat ")) {
        fatcmd_mount(argument(line, sizeof("fat ") - 1));
    } else if (!strcmp(line, "fatls")) {
        fatcmd_ls("/");
    } else if (starts_with(line, "fatls ")) {
        fatcmd_ls(argument(line, sizeof("fatls ") - 1));
    } else if (starts_with(line, "fatcat ")) {
        fatcmd_cat(argument(line, sizeof("fatcat ") - 1));
    } else if (starts_with(line, "fatput ")) {
        fatcmd_put(argument(line, sizeof("fatput ") - 1));
    } else if (starts_with(line, "fatmkdir ")) {
        fatcmd_mkdir(argument(line, sizeof("fatmkdir ") - 1));
    } else if (starts_with(line, "fatrm ")) {
        fatcmd_rm(argument(line, sizeof("fatrm ") - 1));
    } else if (starts_with(line, "fatsum ")) {
        fatcmd_sum(argument(line, sizeof("fatsum ") - 1));
    } else if (starts_with(line, "fatcopy ")) {
        fatcmd_copy(argument(line, sizeof("fatcopy ") - 1));
    } else if (!strcmp(line, "set")) {
        cmd_set("");
    } else if (starts_with(line, "set ")) {
        cmd_set(line + 4);
    } else if (!strcmp(line, "smp")) {
        cmd_smp();
    } else if (!strcmp(line, "smpmem")) {
        smpmem_test();
    } else if (!strcmp(line, "smpthreads")) {
        smpthreads_test();
    } else if (starts_with(line, "cp ")) {
        char *arg = (char *)argument(line, sizeof("cp ") - 1);
        if (!arg)
            printk("cp: give a source and a target\n");
        else
            cmd_cp(arg);
    } else if (starts_with(line, "write ")) {
        char *rest = (char *)argument(line, sizeof("write ") - 1);
        if (rest)
            cmd_write(rest);
        else
            printk("write: give a name and the text\n");
    } else if (!strcmp(line, "desktop")) {
        /* Switching to graphics mode is one-way: from this moment the
           desktop takes over the keyboard and the text console stops
           being visible */
        printk("The desktop is starting - the console goes away\n");
        desktop_start();
    } else if (!strcmp(line, "gfx")) {
        cmd_gfx();
    } else if (!strcmp(line, "ifconfig")) {
        net_ifconfig();
    } else if (!strcmp(line, "arp")) {
        net_arp_dump();
    } else if (starts_with(line, "dns ")) {
        /* Setting the name server by hand - DHCP supplies one, but it is
           not always the one that was meant */
        const char *addr = argument(line, 4);
        uint32_t ip = 0;
        if (!addr || !net_parse_ip(addr, &ip)) {
            printk("usage: dns <server ip address>\n");
        } else {
            net_set_dns(ip);
            printk("DNS server set to ");
            net_print_ip(ip);
            printk("\n");
        }
    } else if (starts_with(line, "ping ")) {
        const char *addr = argument(line, sizeof("ping ") - 1);
        uint32_t ip = 0;
        if (!addr)
            printk("ping: give an address\n");
        else if (!net_parse_ip(addr, &ip))
            printk("ping: '%s' is not an IP address\n", addr);
        else
            net_ping(ip, 3);
    } else if (!strcmp(line, "disk")) {
        cmd_disk();
    } else if (!strcmp(line, "uptime")) {
        cmd_uptime();
    } else if (!strcmp(line, "date")) {
        cmd_date();
    } else if (!strcmp(line, "clear")) {
        vga_clear();
    } else if (!strcmp(line, "screen")) {
        /* What the screen is, and how much of the last frame had to
           cross the bus to the card. */
        printk("mode:            %dx%d, %s, scale %d\n", video_w(), video_h(),
               video_true_colour() ? "true colour" : "256-colour palette", video_scale());
        printk("last frame:      %u rows to the card (of %d)\n",
               video_present_rows(), video_h());
    } else if (!strcmp(line, "reboot")) {
        printk("Restarting...\n");
        /* When the ACPI tables have no reset register, there is a way
           older than they are: a pulse through the keyboard
           controller. */
        if (!acpi_reboot())
            outb(0x64, 0xFE);
    } else if (!strcmp(line, "poweroff")) {
        printk("Powering off...\n");
        acpi_poweroff();
        printk("poweroff: it did not work - "
               "the power can be cut by hand\n");
    } else if (starts_with(line, "runw ")) {
        /* Like 'run', but the program may run on any core - and we do not
           wait for it, so that several can be started at once. Its
           kernel side runs under the big lock anyway; what goes in
           parallel is the counting in ring 3. */
        const char *name = argument(line, 5);
        if (!name) {
            printk("runw: give a program name (see 'ls')\n");
            return;
        }
        static char progw[16];
        int pw = 0;
        while (name[pw] && name[pw] != ' ' && pw < (int)sizeof(progw) - 1) {
            progw[pw] = name[pw];
            pw++;
        }
        progw[pw] = '\0';
        if (!prog_exists(progw)) {
            printk("runw: there is no program '%s'\n", progw);
            return;
        }
        if (proc64_spawn_anycore(progw, 0, 1) < 0)
            printk("runw: could not start '%s'\n", progw);
    } else if (starts_with(line, "run ")) {
        const char *name = argument(line, sizeof("run ") - 1);
        if (!name) {
            printk("run: give a program name (see 'ls')\n");
            return;
        }
        /* The name is the first word, the rest are arguments */
        static char prog[16];
        int pn = 0;
        while (name[pn] && name[pn] != ' ' && pn < (int)sizeof(prog) - 1) {
            prog[pn] = name[pn];
            pn++;
        }
        prog[pn] = '\0';
        name = prog;

        if (!prog_exists(name)) {
            printk("run: there is no program '%s' "
                   "(see 'ls' and 'fls')\n", name);
            return;
        }
        /* The rest of the line is the program's arguments - the shell
           only splits it into words, and what comes of them is the
           program's business */
        static struct args64 args;
        args.argc = 0;
        for (int i = 4; line[i] && args.argc < ARG_MAX; ) {
            while (line[i] == ' ')
                i++;
            if (!line[i])
                break;
            int k = 0;
            while (line[i] && line[i] != ' ' && k < ARG_LEN - 1)
                args.v[args.argc][k++] = line[i++];
            args.v[args.argc][k] = '\0';
            args.argc++;
            while (line[i] && line[i] != ' ')
                i++;
        }

        uint32_t frames_before = pmm_used_frames();
        uint32_t pages_before = isr64_heap_pages();
        uint32_t stack_before = isr64_stack_pages();
        uint32_t files_before = isr64_mmap_pages();
        int id = proc64_spawn(name, args.argc ? &args : 0, 0);
        if (id < 0) {
            printk("run: could not start '%s'\n", name);
            return;
        }
        /* The shell waits for its child - exactly as a program would do
           through a system call. It does not respond in the meantime,
           and that is how it should be. */
        keyboard64_set_intr_target(id);   /* from now on Ctrl+C goes to it */
        int code = 0;
        int done = task64_wait(&code);
        keyboard64_set_intr_target(-1);
        if (done < 0)
            printk("run: the child was lost\n");
        else
            printk("[%s] done, exit code %d (frames %u -> %u, "
                   "heap pages %u, stack %u, from files %u)\n",
                   name, code, frames_before, pmm_used_frames(),
                   isr64_heap_pages() - pages_before,
                   isr64_stack_pages() - stack_before,
                   isr64_mmap_pages() - files_before);
    } else {
        printk("Unknown command: %s (try 'help')\n", line);
    }
}

static void shell_thread(void *arg)
{
    int vc = (int)(long)arg;
    task64_set_vc(vc);          /* from now on we write on our own console */

    /* The command line belongs to the thread, not to the kernel: every
       console has its own shell, so every one has its own buffer too -
       on its own stack, with no tables indexed by console number. */
    char line[LINE_MAX];
    int line_len = 0;

    /* The first console gets a full greeting with the command list, the
       others a short note: four listings in a row on the serial port
       are of no use to anyone. */
    if (vc == 0) {
        printk("\n");
        cmd_help();
    } else {
        printk("ThreadOS 64 - console %d. Alt+F1..F4 switches.\n", vc + 1);
    }
    prompt();

    for (;;) {
        char c = keyboard64_getchar();

        if (c == '\n') {
            printk("\n");
            line[line_len] = '\0';
            execute(line);
            line_len = 0;
            prompt();
        } else if (c == '\b') {
            if (line_len) {
                line_len--;
                printk("\b");
            }
        } else if (c == KEY_COPY) {
            /* The clipboard is the system's, not one window's */
            if (line_len)
                clip_set(line, (uint32_t)line_len);
        } else if (c == KEY_PASTE) {
            char got[CLIP_MAX + 1];
            uint32_t n = clip_get(got, sizeof(got));
            for (uint32_t i = 0; i < n && line_len < LINE_MAX - 1; i++) {
                if (got[i] < ' ' || got[i] > 0x7E)
                    continue;
                line[line_len++] = got[i];
                printk("%c", got[i]);
            }
        } else if (c >= ' ' && c < 0x7F && line_len < LINE_MAX - 1) {
            line[line_len++] = c;
            printk("%c", c);
        }
        /* Control keys (arrows, Ctrl+C) are skipped by the shell for now */
    }
}

void shell64_start(void)
{
    for (int vc = 0; vc < CONSOLE_COUNT; vc++)
        task64_create("shell", shell_thread, (void *)(long)vc);
}
