/* The kernel's shell
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
#include "../kernel/include/heap.h"
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
#include "../kernel/include/devices.h"
#include "../kernel/include/uvc.h"
#include "../kernel/include/power.h"
#include "../kernel/include/hda.h"
#include "../kernel/include/bt.h"
#include "../kernel/include/iwlwifi.h"
#include "../kernel/include/fbcon.h"
#include "../kernel/include/net.h"
#include "../kernel/include/tcp.h"
#include "../kernel/include/video.h"
#include "../kernel/include/desktop.h"
#include "../kernel/include/vga.h"
#include "../kernel/include/mouse.h"

/* As long as a program's whole argument may be (ARG_LEN): a URL with a
   query, handed to curl, is past eighty characters soon enough */
#define LINE_MAX 256

static int sudo_one_shot;
static void read_secret(char *out, int max);

static int confirm_admin_action(const char *action)
{
    if (user_shell_level() != USER_ADMIN) {
        printk("%s: admin access required; try it through sudo\n", action);
        return 0;
    }
    if (sudo_one_shot)
        return 1;

    char password[USER_PASSWORD_MAX + 1];
    printk("%s: admin password: ", action);
    read_secret(password, sizeof(password));
    int ok = user_reauth(password);
    memset(password, 0, sizeof(password));
    if (!ok)
        printk("%s: wrong admin password\n", action);
    return ok;
}

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
    printk("          run <program>, clear, df, format [N]\n");
    printk("          ifconfig, arp, netstat, ping <address>, dns <ip>,\n");
    printk("          gfx, desktop,\n");
    printk("          reboot, poweroff, smp, smpmem, smpthreads\n");
    printk("          runw <prog> - a program on any core\n");
    printk("          whoami, sudo <command>, logout, passwd\n");
    printk("          kill <pid> - end a program (its number from ps)\n");
    printk("          login <account>\n");
    printk("          users, useradd [admin] <name>, userdel <name>\n");
    printk("          fat [N [disk]] - the FAT partition: fatls, fatcat,\n");
    printk("            fatsum, fatput, fatcopy, fatmkdir, fatrm\n");
    printk("          hardware - what is in this machine, disks, font\n");
    printk("          devices - every device and what drives it\n");
    printk("          bt [scan|test] - Bluetooth\n");
    printk("          camera [grab <mode> <n>] - the camera, its modes, "
           "frames\n");
    printk("          wifi [scan|test|join|leave] - the Wi-Fi card, the networks\n");
    printk("          beep [Hz] [ms] - sound, volume [0-100|mute]\n");
    printk("          mic [record <s> <file>] - the microphone\n");
    printk("          format [N] - the whole disk, or partition N, as TFS\n");
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

    if (!confirm_admin_action("disk write test"))
        return;

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
    static char path[FS_PATH_MAX];
    if (fs_getpath(task64_cwd(), path, sizeof(path)) < 0)
        printk("pwd: the path is longer than %u bytes\n", FS_PATH_MAX - 1);
    else
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
    static char name[FS_NAME_LEN + 1];
    uint64_t size = 0;
    int dir = 0;
    cmd_pwd();
    for (int i = 0; fs_readdir(task64_cwd(), i, name,
                               &size, &dir) == 1; i++)
        printk("  %s%s  (%u B)\n", name, dir ? "/" : "", (uint32_t)size);

    uint64_t used = 0, total = 0;
    fs_usage(&used, &total);
    printk("Used: %u/%u blocks of %u B\n", (uint32_t)used, (uint32_t)total,
           fs_block_size());
}

static void cmd_cat(const char *name)
{
    /* On the stack, not static: four consoles have a shell each, and a
       cat on one of them must not print what another one read */
    char buf[512];

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
/* Where the pointer is. A test reads this line to see where a movement
   of the hand put the pointer (scripts/tests/t_mouse.py), so its form
   stays as it is. */
static void cmd_mouse(void)
{
    int x, y, b;
    mouse_get(&x, &y, &b);
    printk("mouse: x=%u y=%u buttons=%u wheel=%s\n", (uint32_t)x,
           (uint32_t)y, (uint32_t)b, mouse_has_wheel() ? "yes" : "no");
}

/* 'wifi': how far the Wi-Fi driver got, and what it knows. The driver
   reports in the boot log too; this is for asking again later. */
static void cmd_wifi(void)
{
    const struct iwlwifi_state *s = iwlwifi_state();
    if (s->stage == IWL_STAGE_NO_CARD || s->stage == IWL_STAGE_NONE) {
        printk("wifi: %s\n", s->stage == IWL_STAGE_NO_CARD
               ? "there is no Intel Wireless 3160 in this machine"
               : "the driver has not started");
        return;
    }
    printk("wifi: Intel Wireless 3160 (8086:%x) at %x:%x.%x, hw rev 0x%x\n",
           (uint32_t)s->device, (uint32_t)s->bus, (uint32_t)s->slot,
           (uint32_t)s->func, s->hw_rev);
    printk("  state:    %s%s\n", iwlwifi_stage_name(s->stage),
           s->rfkill ? " (radio switch off)" : "");
    if (s->fw_version[0])
        printk("  firmware: %s\n", s->fw_version);
    if (s->mac_valid) {
        const uint8_t *m = s->mac;
        printk("  MAC:      %x:%x:%x:%x:%x:%x\n",
               m[0], m[1], m[2], m[3], m[4], m[5]);
        printk("  NVM:      version 0x%x, SKU 0x%x; %d channels "
               "(2.4 GHz %d, 5 GHz %d)\n", (uint32_t)s->nvm_version,
               (uint32_t)s->sku, s->n_channels, s->n_24ghz, s->n_5ghz);
    }
    if (s->calib_results)
        printk("  calibration: %d results\n", s->calib_results);
    if (s->last_error[0])
        printk("  last error: %s\n", s->last_error);
    static const char *const link[] = { "not joined", "joining",
                                         "associated, exchanging keys",
                                         "joined" };
    if (s->stage == IWL_STAGE_READY)
        printk("  network:  %s\n", link[iwlwifi_link_state() & 3]);
    if (s->stage == IWL_STAGE_READY)
        printk("( 'wifi scan' - the networks around; 'wifi join <name> "
               "[password]' )\n");
}

/* 'wifi join <name> [password]': a name with spaces goes in quotes */
static void cmd_wifi_join(const char *arg)
{
    static char ssid[33], password[64];
    uint32_t n = 0;
    while (*arg == ' ')
        arg++;
    if (*arg == '"') {
        arg++;
        while (*arg && *arg != '"' && n < 32)
            ssid[n++] = *arg++;
        if (*arg == '"')
            arg++;
    } else {
        while (*arg && *arg != ' ' && n < 32)
            ssid[n++] = *arg++;
    }
    ssid[n] = 0;
    while (*arg == ' ')
        arg++;
    n = 0;
    while (*arg && n < 63)
        password[n++] = *arg++;
    password[n] = 0;
    if (!ssid[0]) {
        printk("usage: wifi join <name> [password]\n");
        return;
    }
    printk("wifi: joining %s...\n", ssid);
    int r = iwlwifi_join(ssid, password);
    if (r == IWL_OK)
        printk("wifi: joined %s\n", ssid);
    else
        printk("wifi: not joined: %s\n", iwlwifi_state()->last_error);
}

/* 'wifi scan': every channel the card may use, and what was heard */
static void cmd_wifi_scan(void)
{
    static const char *const kind[] = { "open", "WEP", "WPA", "WPA2" };
    printk("wifi: scanning...\n");
    int n = iwlwifi_scan();
    if (n < 0) {
        const struct iwlwifi_state *s = iwlwifi_state();
        printk("wifi: no scan (%s)\n", s->stage != IWL_STAGE_READY
               ? iwlwifi_stage_name(s->stage)
               : s->last_error[0] ? s->last_error : "it did not finish");
        return;
    }
    static struct iwlwifi_bss list[IWLWIFI_MAX_BSS];
    int got = iwlwifi_bss(list, IWLWIFI_MAX_BSS);
    for (int i = 0; i < got; i++) {
        const uint8_t *b = list[i].bssid;
        printk("  %x:%x:%x:%x:%x:%x  ch %u  %d dBm  %s  %s\n", b[0], b[1],
               b[2], b[3], b[4], b[5], (uint32_t)list[i].channel,
               (int)list[i].rssi, kind[list[i].security & 3],
               list[i].ssid_len ? list[i].ssid : "(hidden)");
    }
    printk("wifi: networks heard: %d\n", n);
}

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

    if (starts_with(arg, "pair ")) {
        const char *who = arg + 5;
        while (*who == ' ')
            who++;
        if (bt_pair_start(who) < 0)
            printk("bt: cannot pair with '%s' now (busy, not a classic "
                   "device, or not heard in the last 'bt scan')\n", who);
        return;
    }
    if (!strcmp(arg, "yes") || !strcmp(arg, "no")) {
        if (bt_pair_state() != BT_PAIR_CONFIRM)
            printk("bt: no pairing is waiting for an answer\n");
        else
            bt_pair_answer(arg[0] == 'y');
        return;
    }
    if (starts_with(arg, "forget ")) {
        printk(bt_forget(arg + 7) == 0 ? "bt: forgotten\n"
                                       : "bt: not a paired device\n");
        return;
    }

    if (arg[0] != 's') {                 /* status */
        printk("adapter: %s\n", bt_address());
        printk("name:    %s\n", bt_name());
        printk("version: %s\n", bt_version());
        static char names[8][32], addrs[8][18];
        int np = bt_paired_list(names, addrs, 8);
        for (int i = 0; i < np; i++)
            printk("paired:  %s  %s\n", addrs[i], names[i]);
        printk("( 'bt scan' - who is within reach; 'bt pair <address|n>' )\n");
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
        if (c < 0x20 || c == 0x7F)
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
        if (c < 0x20 || c == 0x7F)
            continue;
        if (n < max - 1) {
            out[n++] = c;
            printk("%c", c);
        }
    }
    out[n] = 0;
    printk("\n");
}

/* Formatting, and the asking that has to come before it.
 *
 * The startup path will not touch a disk that has anything on it, and
 * says so with a pointer to this command - so this is how a disk with a
 * Windows on it becomes a disk with TFS on it, or how one partition of
 * it does while the rest stays. It is deliberately a thing one has to
 * mean: the confirmation is a word typed out, not a key. */
static void cmd_format(const char *arg)
{
    if (!confirm_admin_action("format"))
        return;
    if (!blk_present()) {
        printk("format: there is no disk\n");
        return;
    }

    int which = 0;                 /* 0 = the whole disk */
    for (const char *p = arg; *p >= '0' && *p <= '9'; p++)
        which = which * 10 + (*p - '0');

    printk("Disk: %s, %u MiB\n", blk_name(), blk_sectors() / 2048);
    int np = part_scan();
    if (np > 0) {
        printk("It has %u partition(s) - a %s table:\n", (uint32_t)np,
               part_is_gpt() ? "GPT" : "MBR");
        for (int i = 0; i < np; i++) {
            struct partition pt;
            if (!part_get(i, &pt))
                break;
            printk("  %u: %u MiB, %s %s\n", (uint32_t)(i + 1),
                   (uint32_t)(pt.sectors / 2048), pt.kind, pt.label);
        }
    }

    if (which > 0) {
        struct partition pt;
        if (which > np || !part_get(which - 1, &pt)) {
            printk("format: there is no partition %u\n", (uint32_t)which);
            return;
        }
        printk("\nTHIS WILL ERASE PARTITION %u (%u MiB, %s %s).\n",
               (uint32_t)which, (uint32_t)(pt.sectors / 2048), pt.kind,
               pt.label);
        printk("The rest of the disk and the partition table stay untouched.\n");
    } else if (np > 0) {
        printk("\nTHIS WILL ERASE THE WHOLE DISK - the partition table "
               "and ALL %u partition(s) above.\n", (uint32_t)np);
        printk("To take just one, type 'format N'.\n");
    } else {
        printk("\nTHIS WILL ERASE THE WHOLE DISK, with everything on it.\n");
    }
    printk("Type YES (in capitals) to do it: ");

    char answer[8];
    read_word(answer, (int)sizeof(answer));
    if (strcmp(answer, "YES") != 0) {
        printk("format: nothing was erased\n");
        return;
    }
    if (fs_format_now(which))
        printk("format: done - %s is a ThreadOS TFS now\n",
               which ? "this partition" : "the whole disk");
    else
        printk("format: it did not work\n");
}

static void cmd_df(void)
{
    uint64_t used, total;
    fs_usage(&used, &total);
    if (!total) {
        printk("df: there is no disk\n");
        return;
    }
    uint64_t kib = fs_block_size() / 1024;
    printk("disk: %u KiB, used %u KiB, free %u KiB (blocks of %u B)\n",
           (uint32_t)(total * kib), (uint32_t)(used * kib),
           (uint32_t)((total - used) * kib), fs_block_size());
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
/* 'mic': what is listening; 'mic record <seconds> <file>': that long,
   into a WAV file - for a person checking a microphone, and for tests */
static void put16le(uint8_t *o, uint32_t v)
{
    o[0] = (uint8_t)v;
    o[1] = (uint8_t)(v >> 8);
}

static void put32le(uint8_t *o, uint32_t v)
{
    put16le(o, v);
    put16le(o + 2, v >> 16);
}

static void cmd_mic(const char *arg)
{
    if (!arg || !*arg) {
        printk("mic: %s\n", hda_mic_present() ? hda_mic_name()
                                               : "there is no microphone");
        return;
    }
    if (!starts_with(arg, "record ")) {
        printk("mic: 'mic' or 'mic record <seconds> <file>'\n");
        return;
    }
    const char *a = arg + 7;
    uint32_t secs = 0;
    while (*a >= '0' && *a <= '9')
        secs = secs * 10 + (uint32_t)(*a++ - '0');
    while (*a == ' ')
        a++;
    if (!secs || secs > 10 || !*a) {
        printk("mic: 1 to 10 seconds, and a file\n");
        return;
    }
    uint32_t frames = secs * 48000, bytes = frames * 4;
    uint32_t pages = (44 + bytes + 4095) / 4096;
    uint32_t at = pmm_alloc_contiguous(pages);
    if (!at) {
        printk("mic: no memory for %u seconds\n", secs);
        return;
    }
    uint8_t *wav = (uint8_t *)(uintptr_t)at;
    int r = hda_mic_start(-1);
    if (r < 0) {
        printk("mic: %s\n", r == -2 ? "something is recording already"
                                     : "there is no microphone");
        for (uint32_t p = 0; p < pages; p++)
            pmm_free_frame(at + p * 4096);
        return;
    }
    uint32_t got = 0, idle = 0;
    while (got < frames && idle < 200) {
        int n = hda_mic_read((int16_t *)(void *)(wav + 44 + got * 4),
                             frames - got);
        if (n <= 0) {
            idle++;
            task64_sleep(1);
            continue;
        }
        idle = 0;
        got += (uint32_t)n;
    }
    hda_mic_stop();
    /* The RIFF header: PCM, two channels, 48 kHz, 16 bits */
    memcpy(wav, "RIFF", 4);
    put32le(wav + 4, 36 + got * 4);
    memcpy(wav + 8, "WAVEfmt ", 8);
    put32le(wav + 16, 16);
    put16le(wav + 20, 1);
    put16le(wav + 22, 2);
    put32le(wav + 24, 48000);
    put32le(wav + 28, 48000 * 4);
    put16le(wav + 32, 4);
    put16le(wav + 34, 16);
    memcpy(wav + 36, "data", 4);
    put32le(wav + 40, got * 4);
    int slot = fs_open(task64_cwd(), a, 1);
    if (slot < 0 || fs_truncate(slot) < 0 ||
        fs_write_at(slot, 0, wav, 44 + got * 4) < 0)
        printk("mic: could not write %s\n", a);
    else
        printk("mic: recorded %u frames into %s\n", got, a);
    for (uint32_t p = 0; p < pages; p++)
        pmm_free_frame(at + p * 4096);
}

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

    if (user_shell_level() != USER_ADMIN) {
        printk("install: admin access required; try it through sudo\n");
        return;
    }

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
    /* From live media the typed word is the whole confirmation, as it
       is in the graphical installer: the password guards the system
       this machine already runs, and a live boot has none to guard.
       From an installed system it is asked for again. */
    if (install_update_allowed() && !confirm_admin_action("install"))
        return;
    install_do();
}

static void execute(const char *line);

static void cmd_whoami(void)
{
    printk("%s (%s)\n", user_current_name(),
           user_shell_level() == USER_ADMIN ? "admin" : "user");
}

static void cmd_login(const char *name)
{
    char password[USER_PASSWORD_MAX + 1];
    if (!name || !*name) {
        printk("usage: login <account>\n");
        return;
    }
    printk("password: ");
    read_secret(password, sizeof(password));
    int rc = user_login(name, password);
    memset(password, 0, sizeof(password));
    /* The password was right but the account has nowhere to start:
       saying "invalid password" here would send the person to retype
       the one thing that was correct. */
    if (rc == USER_LOGIN_NO_HOME) {
        printk("login: '%s' cannot sign in - its home folder cannot be opened\n",
               name);
        return;
    }
    if (rc < 0) {
        printk("login: invalid account or password\n");
        return;
    }
    printk("logged in as %s\n", user_current_name());
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

    char pass[USER_PASSWORD_MAX + 1];
    printk("password: ");
    read_secret(pass, sizeof(pass));
    int ok = user_password_ok(pass);
    memset(pass, 0, sizeof(pass));
    if (!ok) {
        printk("sudo: wrong password\n");
        return;
    }

    user_shell_set(USER_ADMIN);
    if (!*arg) {
        printk("you are admin; 'logout' ends it\n");
        return;
    }
    int old_one_shot = sudo_one_shot;
    sudo_one_shot = 1;
    execute(arg);
    sudo_one_shot = old_one_shot;
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

/* Changing a password is sensitive, even in an admin session, so prove
   the current account's password again before replacing it. */
static void cmd_password(void)
{
    char pass[USER_PASSWORD_MAX + 1], again[USER_PASSWORD_MAX + 1];

    printk("current password: ");
    read_secret(pass, sizeof(pass));
    if (!user_password_current_ok(pass)) {
        memset(pass, 0, sizeof(pass));
        memset(again, 0, sizeof(again));
        printk("passwd: wrong password\n");
        return;
    }
    memset(pass, 0, sizeof(pass));
    printk("new password: ");
    read_secret(pass, sizeof(pass));
    if (!pass[0]) {
        memset(pass, 0, sizeof(pass));
        memset(again, 0, sizeof(again));
        printk("passwd: an empty password is no password - not changing it\n");
        return;
    }
    printk("again: ");
    read_secret(again, sizeof(again));
    if (strcmp(pass, again)) {
        memset(pass, 0, sizeof(pass));
        memset(again, 0, sizeof(again));
        printk("passwd: those two are not the same - not changing it\n");
        return;
    }
    if (user_set_password(pass) < 0) {
        memset(pass, 0, sizeof(pass));
        memset(again, 0, sizeof(again));
        printk("passwd: could not save it (no disk?)\n");
        return;
    }
    memset(pass, 0, sizeof(pass));
    memset(again, 0, sizeof(again));
    printk("password changed\n");
}

static void cmd_users(void)
{
    char name[USER_NAME_MAX + 1];
    int uid, level;
    for (int i = 0; user_account_info(i, name, sizeof(name), &uid, &level) == 0;
         i++)
        printk("%s  %s  uid %d\n", name,
               level == USER_ADMIN ? "admin" : "user", uid);
}

static void cmd_useradd(const char *arg)
{
    int level = USER_PLAIN;
    if (starts_with(arg, "admin ")) {
        level = USER_ADMIN;
        arg += 6;
    }
    while (*arg == ' ') arg++;
    if (!*arg || user_shell_level() != USER_ADMIN) {
        printk("usage: sudo useradd [admin] <name>\n");
        return;
    }
    char admin[USER_PASSWORD_MAX + 1], pass[USER_PASSWORD_MAX + 1];
    char again[USER_PASSWORD_MAX + 1];
    printk("admin password: ");
    read_secret(admin, sizeof(admin));
    printk("new account password: ");
    read_secret(pass, sizeof(pass));
    printk("again: ");
    read_secret(again, sizeof(again));
    if (strcmp(pass, again)) {
        memset(admin, 0, sizeof(admin));
        memset(pass, 0, sizeof(pass));
        memset(again, 0, sizeof(again));
        printk("useradd: passwords do not match\n");
        return;
    }
    int rc = user_account_add(arg, pass, level, admin);
    memset(admin, 0, sizeof(admin));
    memset(pass, 0, sizeof(pass));
    memset(again, 0, sizeof(again));
    if (rc < 0) {
        printk("useradd: could not create account\n");
        return;
    }
    printk("account created\n");
}

static void cmd_userdel(const char *name)
{
    if (!*name || user_shell_level() != USER_ADMIN) {
        printk("usage: sudo userdel <name>\n");
        return;
    }
    char admin[USER_PASSWORD_MAX + 1];
    printk("admin password: ");
    read_secret(admin, sizeof(admin));
    int rc = user_account_delete(name, admin);
    memset(admin, 0, sizeof(admin));
    if (rc < 0)
        printk("userdel: could not remove account\n");
    else
        printk("account removed; its home directory was archived\n");
}

static void execute(const char *line)
{
    if (!line[0])
        return;

    if (!strcmp(line, "whoami")) {
        cmd_whoami();
        return;
    }
    if (starts_with(line, "login ")) {
        cmd_login(line + 6);
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
    if (!strcmp(line, "users")) {
        cmd_users();
        return;
    }
    if (starts_with(line, "useradd ")) {
        cmd_useradd(line + 8);
        return;
    }
    if (starts_with(line, "userdel ")) {
        cmd_userdel(line + 8);
        return;
    }

    if (!strcmp(line, "help")) {
        cmd_help();
    } else if (!strcmp(line, "ps")) {
        task64_dump();
    } else if (starts_with(line, "kill ")) {
        /* Ends a program by its number (from 'ps'). As the system call:
           a plain user only their own - the same level and the same
           account */
        const char *a = line + sizeof("kill ") - 1;
        int pid = 0;
        while (*a >= '0' && *a <= '9')
            pid = pid * 10 + (*a++ - '0');
        if (user_shell_level() != USER_ADMIN &&
            (task64_level_of(pid) != user_shell_level() ||
             task64_uid_of(pid) != user_shell_uid()))
            printk("kill: %d is not yours to end\n", pid);
        else if (!task64_signal(pid, 9))
            printk("kill: there is no process %d\n", pid);
        else
            printk("kill: %d ended\n", pid);
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
    } else if (!strcmp(line, "acpi") || !strcmp(line, "battery")) {
        power_report();
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
    } else if (!strcmp(line, "devices")) {
        devices_print();
    } else if (!strcmp(line, "mic")) {
        cmd_mic("");
    } else if (starts_with(line, "mic ")) {
        cmd_mic(line + 4);
    } else if (!strcmp(line, "camera")) {
        uvc_print();
    } else if (starts_with(line, "camera grab ")) {
        /* "camera grab <mode> <n>": for tests, as 'bt test' is */
        const char *a = line + sizeof("camera grab ") - 1;
        int mode = 0, count = 0;
        while (*a >= '0' && *a <= '9')
            mode = mode * 10 + (*a++ - '0');
        while (*a == ' ')
            a++;
        while (*a >= '0' && *a <= '9')
            count = count * 10 + (*a++ - '0');
        uvc_grab(mode, count > 0 ? count : 1);
    } else if (!strcmp(line, "mouse")) {
        cmd_mouse();
    } else if (!strcmp(line, "bt")) {
        cmd_bt("");
    } else if (starts_with(line, "bt ")) {
        cmd_bt(line + 3);
    } else if (!strcmp(line, "wifi")) {
        cmd_wifi();
    } else if (!strcmp(line, "wifi scan")) {
        cmd_wifi_scan();
    } else if (!strcmp(line, "wifi test")) {
        iwlwifi_selftest();
    } else if (starts_with(line, "wifi join ")) {
        cmd_wifi_join(line + sizeof("wifi join ") - 1);
    } else if (!strcmp(line, "wifi leave")) {
        iwlwifi_leave();
        printk("wifi: left the network\n");
    } else if (!strcmp(line, "font")) {
        fonts_command("");
    } else if (starts_with(line, "font ")) {
        fonts_command(line + sizeof("font ") - 1);
    } else if (starts_with(line, "shape ")) {
        gfx_command(line + sizeof("shape ") - 1);
    } else if (!strcmp(line, "disks")) {
        hwinfo_disks();
    } else if (!strcmp(line, "format")) {
        cmd_format("");
    } else if (starts_with(line, "format ")) {
        const char *which = argument(line, sizeof("format ") - 1);
        cmd_format(which ? which : "");
    } else if (starts_with(line, "disks read")) {
        hwinfo_disk_read(line + sizeof("disks read") - 1);
    } else if (!strcmp(line, "df")) {
        cmd_df();
    } else if (!strcmp(line, "netstat")) {
        tcp_dump();
    } else if (!strcmp(line, "crash")) {
        /* A kernel write to an address nothing maps: the page fault
           handler's report, and the stop after it, are what is asked
           for. Canonical, so that it is a page fault and not a general
           protection fault. */
        *(volatile uint32_t *)(uintptr_t)0x00007E0000000000ULL = 1;
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
        if (confirm_admin_action("FAT write"))
            fatcmd_put(argument(line, sizeof("fatput ") - 1));
    } else if (starts_with(line, "fatmkdir ")) {
        if (confirm_admin_action("FAT write"))
            fatcmd_mkdir(argument(line, sizeof("fatmkdir ") - 1));
    } else if (starts_with(line, "fatrm ")) {
        if (confirm_admin_action("FAT write"))
            fatcmd_rm(argument(line, sizeof("fatrm ") - 1));
    } else if (starts_with(line, "fatsum ")) {
        fatcmd_sum(argument(line, sizeof("fatsum ") - 1));
    } else if (starts_with(line, "fatcopy ")) {
        if (confirm_admin_action("FAT write"))
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
        printk("mode:            %dx%d, %s, scale %d%%, panel %dx%d\n",
               video_w(), video_h(),
               video_true_colour() ? "true colour" : "256-colour palette",
               video_scale(), video_pw(), video_ph());
        printk("last frame:      %u rows to the card (of %d)\n",
               video_present_rows(), video_ph());
        int fps, busy, drawn;
        if (desktop_frame_stats(&fps, &busy, &drawn))
            printk("frames:          %d a second, %d%% of it drawing, "
                   "%d of them drawn\n", fps, busy, drawn);
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
        /* Every console has a shell of its own, so nothing a command
           keeps may be static - see 'run' below */
        char progw[16];
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
    } else if (!strcmp(line, "lock")) {
        if (desktop_lock() < 0)
            printk("lock: the desktop is not running\n");
    } else if (starts_with(line, "notify ")) {
        desktop_notify("Shell", argument(line, sizeof("notify ") - 1));
    } else if (starts_with(line, "run ")) {
        const char *name = argument(line, sizeof("run ") - 1);
        if (!name) {
            printk("run: give a program name (see 'ls')\n");
            return;
        }
        /* The name is the first word, the rest are arguments. On this
           thread's stack: the name is printed again once the program
           has ended, and meanwhile another console's shell may have
           run a program of its own. */
        char prog[16];
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
           program's business. Four kilobytes are too many for the
           shell's stack, so they are borrowed from the heap; the spawn
           copies them before it returns. */
        struct args64 *args = kmalloc(sizeof(*args));
        if (!args) {
            printk("run: no memory for the arguments\n");
            return;
        }
        args->argc = 0;
        for (int i = 4; line[i] && args->argc < ARG_MAX; ) {
            while (line[i] == ' ')
                i++;
            if (!line[i])
                break;
            int k = 0;
            while (line[i] && line[i] != ' ' && k < ARG_LEN - 1)
                args->v[args->argc][k++] = line[i++];
            args->v[args->argc][k] = '\0';
            args->argc++;
            while (line[i] && line[i] != ' ')
                i++;
        }

        uint32_t frames_before = pmm_used_frames();
        uint32_t pages_before = isr64_heap_pages();
        uint32_t stack_before = isr64_stack_pages();
        uint32_t files_before = isr64_mmap_pages();
        int id = proc64_spawn(name, args->argc ? args : 0, 0);
        kfree(args);
        if (id < 0) {
            printk("run: could not start '%s'\n", name);
            return;
        }
        /* The shell waits for its child - THIS child. A program started
           with 'runw' is the shell's child too, and nobody waits for it;
           waiting for "any" child took the first of those to end for
           this one, and gave the prompt back while this one still ran.
           It does not respond in the meantime, and that is how it
           should be. */
        keyboard64_set_intr_target(id);   /* from now on Ctrl+C goes to it */
        int code = 0;
        int done;
        /* A program started with 'runw' is this shell's child too, and
           nobody waits for it - so its exit may be the first one found.
           Taking it would report its code as this program's and give
           the prompt back while this one still runs. */
        do
            done = task64_wait(&code);
        while (done >= 0 && done != id);
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
