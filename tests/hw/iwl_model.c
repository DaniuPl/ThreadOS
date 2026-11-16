/* The Intel Wireless 3160 driver against a model of the card, on the host
 *
 * No emulator has this card, so it is written here instead - from the two
 * specifications the driver was written from (the PCIe transport and the
 * firmware's init sequence), not from the driver - and the driver
 * (kernel/iwlwifi.c, compiled as it is, with the sanitizers) is run
 * against it. The card is in tests/hw/iwl/card.c (registers, NIC access,
 * DMA, the receive ring, the command queue) and tests/hw/iwl/fw.c (the
 * firmware file, read by the model itself, and the firmware's side of the
 * conversation: ALIVE, NVM, calibration and the PHY DB, the runtime
 * bring-up). This file is the rest of the machine - PCI, memory, the
 * initrd, time, the kernel log - and the checks.
 *
 * The card moves only when the driver calls into the machine: every MMIO
 * access, io_wait, pit_ticks and task_sleep is card time passing. Nothing
 * runs behind the driver's back, so every run is the same.
 *
 * Usage: iwl_model <scenario> <iwlwifi-3160-17.ucode>
 *   ok       - everything right: the whole bring-up, then 300 notifications
 *              through the receive ring with the driver polling
 *   nofw-ack - the card never signals a firmware chunk done
 *   assert   - the runtime firmware crashes at its first command
 *   rfkill   - the radio switch is off
 *   scan     - the bring-up, then a scan (stage 2): the command checked
 *              field by field, seven frames heard, five networks
 *   nofile   - the initrd has no firmware file: the Device Manager is
 *              to say the card needs firmware, not that it failed
 * Prints "ok: ..." / "FAIL: ..." per check, "note: ..." where the
 * specifications leave a choice and the model took one.
 */

#define _GNU_SOURCE
#include <fcntl.h>
#include <regex.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "kernel/include/iwlwifi.h"
#include "kernel/include/pci.h"
#include "kernel/include/device_info.h"
#include "iwl/card.h"
#include "devices_rec.h"

int scenario;
int bar_mapped;

/* --- checks ---------------------------------------------------------------- */

static int failures;

static void check(const char *what, int ok)
{
    printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok)
        failures++;
}

/* --- the kernel log ---------------------------------------------------------- */

static char logbuf[1 << 18];
static size_t loglen;
static char linebuf[1024];
static size_t linelen;

static void log_char(char c)
{
    if (loglen + 1 < sizeof(logbuf)) {
        logbuf[loglen++] = c;
        logbuf[loglen] = 0;
    }
    if (c == '\n' || linelen + 1 >= sizeof(linebuf)) {
        linebuf[linelen] = 0;
        printf("  | %s\n", linebuf);
        linelen = 0;
        return;
    }
    linebuf[linelen++] = c;
}

static void log_str(const char *s)
{
    if (!s)
        s = "(null)";
    while (*s)
        log_char(*s++);
}

static void log_uint(uint64_t v, unsigned base)
{
    char b[24];
    int i = sizeof(b) - 1;
    b[i] = 0;
    do {
        b[--i] = "0123456789abcdef"[v % base];
        v /= base;
    } while (v);
    log_str(b + i);
}

/* kernel/printk.c's formatting, and only that */
void printk(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            log_char(*fmt);
            continue;
        }
        switch (*++fmt) {
        case 's': log_str(va_arg(ap, const char *)); break;
        case 'c': log_char((char)va_arg(ap, int)); break;
        case 'd': {
            int v = va_arg(ap, int);
            if (v < 0) {
                log_char('-');
                log_uint((uint32_t)-(int64_t)v, 10);
            } else {
                log_uint((uint32_t)v, 10);
            }
            break;
        }
        case 'u': log_uint(va_arg(ap, uint32_t), 10); break;
        case 'x': log_uint(va_arg(ap, uint32_t), 16); break;
        case 'p':
            log_str("0x");
            log_uint((uintptr_t)va_arg(ap, void *), 16);
            break;
        case '%': log_char('%'); break;
        case 0:
            va_end(ap);
            return;
        default:
            log_char('%');
            log_char(*fmt);
        }
    }
    va_end(ap);
}

/* Where in the log a pattern first matches (-1: nowhere) */
static long log_find(const char *pattern)
{
    regex_t re;
    regmatch_t m;
    if (regcomp(&re, pattern, REG_EXTENDED | REG_ICASE | REG_NEWLINE)) {
        printf("FAIL: the model's own pattern does not compile: %s\n",
               pattern);
        exit(3);
    }
    long at = regexec(&re, logbuf, 1, &m, 0) ? -1 : (long)m.rm_so;
    regfree(&re);
    return at;
}

/* The line that matched, for the message */
static const char *log_line(long at)
{
    static char l[256];
    if (at < 0)
        return "(none)";
    const char *s = logbuf + at;
    while (s > logbuf && s[-1] != '\n')
        s--;
    size_t n = strcspn(s, "\n");
    if (n >= sizeof(l))
        n = sizeof(l) - 1;
    memcpy(l, s, n);
    l[n] = 0;
    return l;
}

/* --- PCI ------------------------------------------------------------------- */

static uint8_t cfg[256];
static uint8_t cfg40_orig[4];
static int bar_sizing;

static void cfg32(int off, uint32_t v)
{
    for (int i = 0; i < 4; i++)
        cfg[off + i] = (uint8_t)(v >> (8 * i));
}

static uint32_t cfg_rd(int off)
{
    return cfg[off] | cfg[off + 1] << 8 | cfg[off + 2] << 16 |
           (uint32_t)cfg[off + 3] << 24;
}

static void pci_init(void)
{
    cfg32(0x00, 0x08B38086u);
    /* As the firmware leaves it: memory decoding on, no bus mastering */
    cfg32(0x04, 0x00100002u);
    cfg32(0x08, 0x02800083u);              /* network controller, rev 0x83 */
    cfg32(0x10, (uint32_t)BAR0_PHYS | 0x4); /* 64-bit memory BAR */
    cfg32(0x14, (uint32_t)(BAR0_PHYS >> 32));
    cfg32(0x2C, 0x80708086u);              /* subsystem 8086:8070 (2ac) */
    cfg[0x34] = 0xC8;
    cfg32(0x3C, 0x0000010Bu);
    /* Vendor bytes around the retry timeout (0x41) */
    cfg32(0x40, 0x56348012u);
    memcpy(cfg40_orig, cfg + 0x40, 4);
    /* Capabilities: power management, MSI, PCI Express */
    cfg32(0xC8, 0x0003D001u);
    cfg32(0xD0, 0x00808005u);
    cfg32(0x80, 0x00020010u);
    cfg32(0x84, 0x00008DC0u);
    cfg32(0x88, 0x00002810u);
    cfg32(0x8C, 0x0003EC11u);
    cfg32(0x90, 0x10110042u);              /* LNKCTL: ASPM L1 on, L0s off */
    cfg32(0xA8, 0x00000000u);              /* DEVCTL2: LTR off */
}

int pci_mem_enabled(void) { return (cfg[0x04] & 0x2) != 0; }
int pci_bus_master(void) { return (cfg[0x04] & 0x4) != 0; }

static int finds;

int pci_find_device(uint16_t vendor, uint16_t device, struct pci_dev *out)
{
    finds++;
    if (vendor != 0x8086 || device != 0x08B3)
        return 0;
    memset(out, 0, sizeof(*out));
    out->bus = CARD_BUS;
    out->slot = CARD_SLOT;
    out->func = CARD_FUNC;
    out->vendor = vendor;
    out->device = device;
    for (int i = 0; i < 6; i++)
        out->bar[i] = cfg_rd(0x10 + 4 * i);
    out->bar0 = out->bar[0];
    out->irq = cfg[0x3C];
    return 1;
}

void pci_enable_bus_master(const struct pci_dev *dev)
{
    if (dev->bus == CARD_BUS && dev->slot == CARD_SLOT &&
        dev->func == CARD_FUNC)
        cfg[0x04] |= 0x07;
}

static int is_card(int bus, int slot, int func, int off)
{
    if (bus != CARD_BUS || slot != CARD_SLOT || func != CARD_FUNC)
        return 0;
    if (off < 0 || off > 0xFC || (off & 3)) {
        violation("PCI configuration access at 0x%x: dwords below 0x100 "
                  "only", off);
        return 0;
    }
    return 1;
}

uint32_t pci_config_read(int bus, int slot, int func, int off)
{
    if (!is_card(bus, slot, func, off))
        return 0xFFFFFFFFu;
    if (off == 0x10 && bar_sizing)
        return 0xFFFFE004u;                 /* 8 KiB */
    return cfg_rd(off);
}

void pci_config_write(int bus, int slot, int func, int off, uint32_t v)
{
    if (!is_card(bus, slot, func, off))
        return;
    switch (off) {
    case 0x04:
        cfg[0x04] = (uint8_t)v;
        cfg[0x05] = (uint8_t)(v >> 8);
        return;
    case 0x40:
        cfg32(0x40, v);
        return;
    case 0x10:
        if (v == 0xFFFFFFFFu) {
            bar_sizing = 1;
            return;
        }
        bar_sizing = 0;
        if ((v & ~0xFu) != ((uint32_t)BAR0_PHYS))
            violation("BAR0 moved to 0x%08x", v);
        return;
    case 0x14:
        if (v != (uint32_t)(BAR0_PHYS >> 32) && v != 0xFFFFFFFFu)
            violation("BAR1 (BAR0's upper half) written with 0x%08x", v);
        return;
    }
    if (off == 0x90 || off == 0x88 || off == 0xA8)
        note("PCIe control register 0x%x written; SPEC_T 1.2 leaves ASPM "
             "as the firmware set it", off);
    else
        note("PCI configuration 0x%02x written with 0x%08x", off, v);
    cfg32(off, v);
}

int paging64_map_phys(uint64_t phys, uint64_t len)
{
    if (phys <= BAR0_PHYS && phys + len >= BAR0_PHYS + BAR0_SIZE)
        bar_mapped = 1;
    else
        note("paging64_map_phys(0x%llx, 0x%llx) does not cover BAR0",
             (unsigned long long)phys, (unsigned long long)len);
    return 0;
}

/* --- memory ------------------------------------------------------------------ */

#define POOL_FRAMES 8192                     /* 32 MiB */
static uint8_t *pool;
static uint32_t pool_used;
static struct {
    uint64_t addr, len;
} allocs[512];
static int nallocs;

uint32_t pmm_alloc_contiguous(uint32_t n)
{
    if (!pool) {
        pool = mmap((void *)0, (size_t)POOL_FRAMES * 4096,
                    PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
        if (pool == MAP_FAILED) {
            printf("FAIL: no memory below 4 GiB on this host\n");
            exit(3);
        }
    }
    if (n == 0 || pool_used + n > POOL_FRAMES || nallocs >= 512)
        return 0;
    uint8_t *p = pool + (size_t)pool_used * 4096;
    pool_used += n;
    memset(p, 0xA5, (size_t)n * 4096);       /* not zeroed */
    allocs[nallocs].addr = (uintptr_t)p;
    allocs[nallocs].len = (uint64_t)n * 4096;
    nallocs++;
    return (uint32_t)(uintptr_t)p;
}

int dma_ok(uint64_t addr, uint64_t len)
{
    for (int i = 0; i < nallocs; i++)
        if (addr >= allocs[i].addr &&
            addr + len <= allocs[i].addr + allocs[i].len)
            return 1;
    return 0;
}

void *dma_ptr(uint64_t addr)
{
    return (void *)(uintptr_t)addr;
}

/* --- the initrd ---------------------------------------------------------------- */

static const uint8_t *fw_file;
static size_t fw_size;

const void *initrd_find(const char *name, uint32_t *size)
{
    if (scenario == SC_NOFILE)
        return NULL;
    if (strcmp(name, "iwl3160.ucode")) {
        note("initrd_find(\"%s\"): the initrd has iwl3160.ucode", name);
        return NULL;
    }
    if (size)
        *size = (uint32_t)fw_size;
    return fw_file;
}

/* --- time ------------------------------------------------------------------------ */

static jmp_buf escape;
static int armed;
static uint64_t deadline;
static int hung;

void machine_after_advance(void)
{
    if (armed && now_ns > deadline) {
        armed = 0;
        hung = 1;
        longjmp(escape, 1);
    }
}

static int tasks;

struct task *task_create(const char *name, void (*entry)(void *), void *arg)
{
    (void)entry;
    (void)arg;
    tasks++;
    note("task_create(\"%s\") during the bring-up: not run by the model",
         name);
    return (struct task *)(uintptr_t)1;
}

void task_sleep(uint32_t ms)
{
    model_advance(ms ? (uint64_t)ms * MS : 100 * US);
}

uint32_t pit_ticks(void)
{
    model_advance(1 * US);
    return (uint32_t)(now_ns / (10 * MS));
}

void model_io_wait(void)
{
    model_advance(1 * US);
}

/* --- the scenarios' checks ------------------------------------------------------------- */

#define MAC_RE "7c:5c:f8:12:34:56"

/* The channel list the specification derives from the NVM (SPEC_I 5.7) */
static int want_channels(struct iwlwifi_channel *w)
{
    int n = 0;
    for (int i = 0; i < 39; i++) {
        uint16_t f = nvm_chan_flags[i];
        int ch = nvm_chan_num[i];
        if (i >= 14 && !(NVM_SKU & 2))
            continue;
        if (!(f & 0x1))
            continue;
        struct iwlwifi_channel c;
        memset(&c, 0, sizeof(c));
        c.number = (uint16_t)ch;
        c.band = i < 14 ? IWLWIFI_BAND_24 : IWLWIFI_BAND_5;
        c.freq = (uint16_t)(i < 14 ? (ch == 14 ? 2484 : 2407 + 5 * ch)
                                   : 5000 + 5 * ch);
        c.max_power = 22;
        c.nvm_flags = f;
        if (f & 0x200) {
            if (i < 14) {
                if (ch <= 9)
                    c.flags |= IWLWIFI_CHAN_HT40_PLUS;
                if (ch >= 5)
                    c.flags |= IWLWIFI_CHAN_HT40_MINUS;
            } else if (ch <= 165) {
                c.flags |= (i - 14) % 2 == 0 ? IWLWIFI_CHAN_HT40_PLUS
                                             : IWLWIFI_CHAN_HT40_MINUS;
            }
        }
        if (!(f & 0x400))
            c.flags |= IWLWIFI_CHAN_NO_80MHZ;
        if (!(f & 0x800))
            c.flags |= IWLWIFI_CHAN_NO_160MHZ;
        int no_ir = !(f & 0x2) || !(f & 0x8);
        if (no_ir)
            c.flags |= IWLWIFI_CHAN_NO_IR;
        if (f & 0x10)
            c.flags |= IWLWIFI_CHAN_RADAR;
        if (f & 0x20)
            c.flags |= IWLWIFI_CHAN_INDOOR;
        if ((f & 0x40) && no_ir)
            c.flags |= IWLWIFI_CHAN_IR_CONCURRENT;
        w[n++] = c;
    }
    return n;
}

static void check_channels(void)
{
    struct iwlwifi_channel want[39], got[IWLWIFI_MAX_CHANNELS + 1];
    int nw = want_channels(want);
    memset(got, 0, sizeof(got));
    int ng = iwlwifi_channels(got, IWLWIFI_MAX_CHANNELS);
    int same = ng == nw, bad = -1;
    for (int i = 0; same && i < nw; i++)
        if (got[i].number != want[i].number || got[i].freq != want[i].freq ||
            got[i].band != want[i].band ||
            got[i].max_power != want[i].max_power ||
            got[i].nvm_flags != want[i].nvm_flags ||
            got[i].flags != want[i].flags) {
            same = 0;
            bad = i;
        }
    if (bad >= 0)
        printf("  channel %u: the driver has %u MHz band %u power %u nvm "
               "0x%x flags 0x%x; the NVM means %u MHz band %u power %u nvm "
               "0x%x flags 0x%x\n", want[bad].number, got[bad].freq,
               got[bad].band, got[bad].max_power, got[bad].nvm_flags,
               got[bad].flags, want[bad].freq, want[bad].band,
               want[bad].max_power, want[bad].nvm_flags, want[bad].flags);
    else if (ng != nw)
        printf("  the driver lists %d channels, the NVM has %d\n", ng, nw);
    check("the channel list is the NVM's, in its order: 35 channels, their "
          "frequencies, and NO_IR, radar, indoor, HT40+/-, no 80/160 MHz "
          "derived as SPEC_I 5.7 says", same);
}

/* Stage 2. What the scan must come to, written out here from the model's
   networks (tests/hw/iwl/fw.c) rather than read from them: a beacon
   heard twice is one network at its louder, a damaged frame and a data
   frame are none, a hidden network has no name, and the protection is
   RSN = WPA2, Microsoft's element = WPA, the privacy bit alone = WEP. */
static void scenario_scan(int rc)
{
    check("scan: the card came up first", rc == IWL_OK);
    if (rc != IWL_OK)
        return;
    volatile int n = -999;
    deadline = now_ns + 20000 * MS;
    armed = 1;
    if (!setjmp(escape))
        n = iwlwifi_scan_now();
    armed = 0;
    check("scan: it ended, inside 20 s of card time", !hung && n >= 0);
    check("scan: the firmware took the command - SCAN_OFFLOAD_REQUEST_CMD "
          "of the size the file's channel count makes, every field as the "
          "API and a regular iwlmvm scan have it", fs.scan_cmd_ok == 1);
    check("scan: the firmware swept every channel and said it was done",
          fs.scans_done == 1);
    static const struct {
        const char *bssid, *ssid;
        int channel, rssi, sec;
    } want[5] = {
        { "02:7a:00:00:00:01", "ThreadOS-Home", 6, -48, IWLWIFI_SEC_WPA2 },
        { "02:7a:00:00:00:02", "Cafe Guest", 1, -71, IWLWIFI_SEC_OPEN },
        { "02:7a:00:00:00:03", "", 11, -80, IWLWIFI_SEC_WPA2 },
        { "02:7a:00:00:00:04", "OldRouter", 11, -60, IWLWIFI_SEC_WEP },
        { "02:7a:00:00:00:05", "Office", 36, -58, IWLWIFI_SEC_WPA },
    };
    struct iwlwifi_bss got[IWLWIFI_MAX_BSS];
    int ng = iwlwifi_bss(got, IWLWIFI_MAX_BSS);
    check("scan: five networks - the damaged frame and the data frame "
          "are none, the twice-heard one is one", n == 5 && ng == 5);
    int found = 0;
    for (int w = 0; w < 5; w++) {
        int ok = 0;
        for (int i = 0; i < ng && i < IWLWIFI_MAX_BSS; i++) {
            char b[18];
            snprintf(b, sizeof(b), "%02x:%02x:%02x:%02x:%02x:%02x",
                     got[i].bssid[0], got[i].bssid[1], got[i].bssid[2],
                     got[i].bssid[3], got[i].bssid[4], got[i].bssid[5]);
            if (strcmp(b, want[w].bssid))
                continue;
            ok = !strcmp(got[i].ssid, want[w].ssid) &&
                 got[i].ssid_len == strlen(want[w].ssid) &&
                 got[i].channel == want[w].channel &&
                 got[i].rssi == want[w].rssi &&
                 got[i].security == want[w].sec;
            if (!ok)
                printf("  %s: the driver has '%s' ch %u %d dBm sec %u\n", b,
                       got[i].ssid, got[i].channel, got[i].rssi,
                       got[i].security);
        }
        found += ok;
    }
    check("scan: each by its BSSID, with its name (none when hidden), its "
          "channel, its loudest signal and its protection (WPA2, open by "
          "a probe response, WEP, WPA on 5 GHz)", found == 5);
}

/* The milestone lines of the contract, in order */
static const char *milestones[] = {
    "^\\[wifi\\] Intel Wireless 3160 at 0*2:0*0\\.0, hw rev 0x0*165$",
    "^\\[wifi\\] firmware 17\\.bfb58538\\.0: 918268 bytes, init "
    "(65536\\+163840|163840\\+65536) B, runtime (65536\\+163840|163840\\+65536) B$",
    "^\\[wifi\\] INIT image alive \\(status 0xcafe\\)$",
    "^\\[wifi\\] calibration done: 1[56] results$",
    "^\\[wifi\\] NVM: MAC " MAC_RE ", 35 channels \\(2\\.4 GHz 13, 5 GHz 22\\)$",
    "^\\[wifi\\] runtime image alive$",
    "^\\[ OK \\] Wi-Fi: Intel Wireless 3160, MAC " MAC_RE ", firmware ready$",
};
static const char *milestone_names[] = {
    "the card, its location and hw rev",
    "the firmware file: version, size, both images' sections",
    "the INIT image alive, status 0xcafe",
    "calibration done, with the number of results",
    "the NVM: MAC 7c:5c:f8:12:34:56, 35 channels (13 + 22)",
    "the runtime image alive",
    "[ OK ] Wi-Fi ... firmware ready",
};

/* Each line comes after every line of a lower rank. The contract lists
   calibration before the NVM, while SPEC_I 4 reads the NVM before it
   calibrates: those two may come in either order. */
static const int milestone_rank[] = { 0, 1, 2, 3, 3, 4, 5 };

static void milestones_upto(int n)
{
    long at[7];
    for (int i = 0; i < n; i++)
        at[i] = log_find(milestones[i]);
    for (int i = 0; i < n; i++) {
        int ok = at[i] >= 0;
        for (int j = 0; j < n; j++)
            if (milestone_rank[j] < milestone_rank[i] && at[j] >= at[i])
                ok = 0;
        char what[200];
        snprintf(what, sizeof(what), "the log says, in its place: %s",
                 milestone_names[i]);
        check(what, ok);
    }
}

static int pci_checks(void)
{
    check("BAR0 mapped, memory decoding and bus mastering on",
          bar_mapped && pci_mem_enabled() && pci_bus_master());
    check("INTx disabled in the PCI command register: the driver polls "
          "(SPEC_T 1.3)", (cfg[0x05] & 0x04) != 0);
    check("PCI config byte 0x41 (retry timeout) is 0, the bytes beside it "
          "kept (SPEC_T 1.2 step 6)",
          cfg[0x41] == 0 && cfg[0x40] == cfg40_orig[0] &&
          cfg[0x42] == cfg40_orig[2] && cfg[0x43] == cfg40_orig[3]);
    return 0;
}

static long fail_line(void)
{
    return log_find("^\\[ !! \\] Wi-Fi: .*$");
}

static void scenario_ok(int rc)
{
    const struct iwlwifi_state *s = iwlwifi_state();
    check("iwlwifi_run says IWL_OK", rc == IWL_OK);
    pci_checks();
    check("the card prepared before APM init: NIC_READY granted after the "
          "PREPARE handshake the card needed at power-on (SPEC_T 4.1)",
          cs.prepare_seen && cs.ready_granted_after_prepare);
    milestones_upto(7);
    check("no failure line in the log", fail_line() < 0);
    check("the INIT image loaded byte for byte at the file's addresses and "
          "started (CSR_RESET = 0)", fs.init_bytes_ok && fs.init_starts >= 1);
    check("APM init as SPEC_T 4.2 has it before each firmware start "
          "(chicken bits, HPET, HAP_WAKE, OSC_CLK, DMA clock, L1-Active off)",
          cs.apm_ok_at_start);
    check("CSR_INT_COALESCING 0x40 with bit 31 (the 3160 workaround), "
          "shadow registers on, GIO_REG L0S bit set as the link has ASPM L1 "
          "(SPEC_T 4.2 step 5, 4.4 step 6f, 6.2)",
          (cs.coalescing_at_start & 0x800000FFu) == 0x80000040u &&
          cs.shadow_at_start && (cs.gio_at_start & 0x2));
    if (cs.cbbc_programmed != 31)
        note("%d of the 31 TFD rings programmed (SPEC_T 7.3 programs all)",
             cs.cbbc_programmed);
    check("the INIT image calibrated and said INIT_COMPLETE",
          fs.init_calibrated);
    check("the driver read the NVM (NVM_ACCESS_CMD)", fs.nvm_reads >= 3);
    check("between the images the CPU was reset; the runtime image loaded "
          "byte for byte and came alive",
          fs.rt_bytes_ok && fs.rt_starts >= 1 && fs.rt_alive >= 1 &&
          !fs.rt_before_calib);
    check("the runtime image got the PHY DB back exactly as the INIT image "
          "produced it: 15 sections in order, the resent one in its later "
          "form (SPEC_I 6)", fs.phydb_ok);
    fw_rt_report();
    check("the runtime image got the whole minimal-safe bring-up (SPEC_I "
          "7.15): SF_CFG, TX_ANT, BT_CONFIG, PHY DB, PHY_CFG, aux queue and "
          "station, PHY contexts, POWER_TABLE",
          fs.rt_configured && !fs.rt_required_missing);
    check("L1-Active disabled during the load and enabled again after ALIVE "
          "(APMG_PCIDEV_STT bit 11; SPEC_T 4.2 step 10c, 7.4 step 10)",
          cs.l1_disabled_at_start && !(prph_value(0x3010) & 0x800));
    check("the scheduler: SCD_GP_CTRL auto-active and 31 queues (SPEC_T 7.3)",
          (prph_value(0x2DA8) & 0x40001) == 0x40001);
    int set, cleared;
    int ps22 = card_ps22(&set, &cleared);
    note("APMG_PS_CTRL bit 22 (EARLY_PWR_OFF_RESET_DIS) %s: SPEC_T 4.5 sets "
         "it, SPEC_I 1.4 says clear - both accepted", ps22 ? "set" : "clear");

    check("the driver's state: stage READY, the card 0x08B3 at 02:00.0, hw "
          "rev 0x165, firmware 17.bfb58538.0",
          s->stage == IWL_STAGE_READY && s->device == 0x08B3 &&
          s->bus == CARD_BUS && s->slot == CARD_SLOT &&
          s->func == CARD_FUNC && s->hw_rev == CARD_HW_REV &&
          !strcmp(s->fw_version, "17.bfb58538.0"));
    check("the driver's MAC is the NVM's, read the right way round "
          "(7c:5c:f8:12:34:56)",
          s->mac_valid && !memcmp(s->mac, nvm_mac, 6));
    check("the driver's NVM fields: version 0x709, SKU 0xF, radio config "
          "0x1109", s->nvm_version == NVM_VERSION && s->sku == NVM_SKU &&
          s->radio_cfg == NVM_RADIO_CFG);
    check("the driver counts 35 channels, 13 at 2.4 GHz and 22 at 5 GHz",
          s->n_channels == 35 && s->n_24ghz == 13 && s->n_5ghz == 22);
    if (s->calib_results != fs.calib_unique &&
        s->calib_results != fs.calib_sent)
        printf("  calibration results: the driver has %d\n",
               s->calib_results);
    check("the driver counts the calibration results (15 sections, 16 "
          "notifications)", s->calib_results == fs.calib_unique ||
          s->calib_results == fs.calib_sent);
    if (s->calib_results == fs.calib_sent)
        note("calibration results counted per notification (16); the "
             "PHY DB holds 15 - either reading accepted");
    check("no RF kill, no firmware error in the driver's state",
          !s->rfkill && !s->fw_error_id);
    check_channels();
}

static void scenario_ok_after(uint32_t delivered_before, int polled_ok)
{
    uint32_t got = cs.pkts_delivered - delivered_before;
    if (!polled_ok)
        printf("  the driver hung while polling\n");
    printf("  after the bring-up: %d notifications sent, %u packets "
           "delivered, %u receive buffers filled in all\n", fs.flood_sent,
           got, cs.rx_delivered);
    check("after the bring-up, 300 notifications through the 256-buffer "
          "ring while the driver polls: all delivered, the ring wrapped, "
          "every buffer given back (SPEC_T 6.4, 6.5)",
          polled_ok && fs.flood_sent == 300 && got >= 300 &&
          !cs.rx_stalled && card_rx_backlog() == 0);
    check("the firmware idle at the end: every command answered, no crash",
          card_cmds_pending() == 0 && !fs.crashed && fw_running());
    if (cs.mac_access_held)
        note("the driver leaves MAC_ACCESS_REQ set while idle: the NIC "
             "never sleeps");
}

static void scenario_nofw(int rc)
{
    const struct iwlwifi_state *s = iwlwifi_state();
    check("iwlwifi_run returns an error", rc < 0);
    milestones_upto(2);
    long at = fail_line();
    printf("  the driver said: %s\n", log_line(at));
    check("the log says the firmware load failed, and where: the INIT "
          "image's chunk/section",
          at >= 0 &&
          log_find("^\\[ !! \\] Wi-Fi: .*(init.*(chunk|section|load)|"
                   "(chunk|section|load).*init)") >= 0);
    check("one chunk started, never signalled, and nothing after it: no "
          "second chunk, the CPU never released",
          cs.chunks_started == 1 && cs.chunks_unsignalled == 1 &&
          cs.cpu_starts == 0);
    uint64_t waited = now_ns - cs.last_chunk_start;
    printf("  waited %llu ms of card time after the chunk\n",
           (unsigned long long)(waited / MS));
    if (waited < 5000 * MS)
        note("the driver gave up after %llu ms; SPEC_T 5.4 waits 5 s per "
             "chunk", (unsigned long long)(waited / MS));
    check("the driver waited for the chunk (at least 0.5 s) and gave up in "
          "time (within 30 s)", waited >= 500 * MS && now_ns < 30000 * MS);
    check("the driver's state: stage FAILED, with the error kept",
          s->stage == IWL_STAGE_FAILED && s->last_error[0]);
}

static void scenario_assert(int rc)
{
    const struct iwlwifi_state *s = iwlwifi_state();
    check("iwlwifi_run returns an error", rc < 0);
    milestones_upto(6);
    long at = fail_line();
    printf("  the driver said: %s\n", log_line(at));
    check("the runtime firmware crashed at its first command (the "
          "scenario) and the driver read the error log from the address "
          "ALIVE gave", fs.crashed && fs.crash_id == 0x34 &&
          cs.errlog_read >= 5);
    check("the failure line names the error id 0x34 and NMI_INTERRUPT_WDG",
          log_find("^\\[ !! \\] Wi-Fi: .*0x0*34[^0-9a-f].*"
                   "NMI_INTERRUPT_WDG") >= 0 ||
          log_find("^\\[ !! \\] Wi-Fi: .*NMI_INTERRUPT_WDG.*0x0*34"
                   "([^0-9a-f]|$)") >= 0);
    check("... and the log's pc 0x12f4a and blink1 0x1a2b0",
          log_find("^\\[ !! \\] Wi-Fi: .*0x0*12f4a") >= 0 &&
          log_find("^\\[ !! \\] Wi-Fi: .*0x0*1a2b0") >= 0);
    if (log_find("^\\[ !! \\] Wi-Fi: .*0x0*c3d4") < 0)
        note("the failure line does not show blink2 (0xc3d4)");
    check("the driver's state: stage FAILED, fw_error_id 0x34",
          s->stage == IWL_STAGE_FAILED && s->fw_error_id == 0x34);
    check("no [ OK ] line", log_find("^\\[ OK \\] Wi-Fi") < 0);
}

static void scenario_rfkill(int rc)
{
    const struct iwlwifi_state *s = iwlwifi_state();
    check("iwlwifi_run returns an error", rc < 0);
    milestones_upto(2);
    long at = fail_line();
    printf("  the driver said: %s\n", log_line(at));
    check("the failure line says the radio is switched off (RF kill)",
          log_find("^\\[ !! \\] Wi-Fi: .*(rf.?kill|kill switch|radio "
                   "switch|switched off|switch is off)") >= 0);
    check("the runtime image was never started, and the driver stopped in "
          "time (within 20 s of card time)",
          fs.rt_starts == 0 && now_ns < 20000 * MS);
    if (fs.init_starts)
        note("the INIT image ran under RF-kill (%d NVM reads%s)",
             fs.nvm_reads, log_find(milestones[4]) >= 0 ?
             ", the MAC and channels reported" : "");
    else
        note("the driver did not start the INIT image under RF-kill");
    check("the driver's state: stage RFKILL, rfkill set",
          s->stage == IWL_STAGE_RFKILL && s->rfkill);
    check("no [ OK ] line", log_find("^\\[ OK \\] Wi-Fi") < 0);
}

/* --- what the Device Manager is told ------------------------------------------------ */

/* The card's state as Settings' Devices page asks for it: the function
   the driver left with device_live(), called now */
static int device_said(char *why, int n)
{
    why[0] = 0;
    if (dev_rec.claims != 1 || strcmp(dev_rec.driver, "iwlwifi") ||
        !dev_rec.live)
        return -1;
    return dev_rec.live(why, n);
}

static void scenario_nofile(int rc)
{
    check("iwlwifi_run says the firmware is bad", rc == IWL_ERR_BADFW);
    char why[64];
    int state = device_said(why, sizeof(why));
    printf("  the Device Manager is told: state %d, '%s'\n", state, why);
    check("the Device Manager: the card needs firmware, iwl3160.ucode",
          state == DEV_FIRMWARE && strstr(why, "iwl3160.ucode"));
}

/* --- main ------------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    setvbuf(stdout, (char *)0, _IONBF, 0);
    if (argc < 3) {
        fprintf(stderr, "usage: %s ok|nofw-ack|assert|rfkill|scan|nofile "
                "<ucode>\n",
                argv[0]);
        return 2;
    }
    if (!strcmp(argv[1], "ok"))
        scenario = SC_OK;
    else if (!strcmp(argv[1], "nofw-ack"))
        scenario = SC_NOFW_ACK;
    else if (!strcmp(argv[1], "assert"))
        scenario = SC_ASSERT;
    else if (!strcmp(argv[1], "rfkill"))
        scenario = SC_RFKILL;
    else if (!strcmp(argv[1], "scan"))
        scenario = SC_SCAN;
    else if (!strcmp(argv[1], "nofile"))
        scenario = SC_NOFILE;
    else {
        fprintf(stderr, "unknown scenario %s\n", argv[1]);
        return 2;
    }
    int fd = open(argv[2], O_RDONLY);
    struct stat sb;
    if (fd < 0 || fstat(fd, &sb) < 0) {
        printf("FAIL: cannot open the firmware file %s\n", argv[2]);
        return 3;
    }
    fw_size = (size_t)sb.st_size;
    /* Read-only, as the initrd is to a driver: writing it faults */
    fw_file = mmap(NULL, fw_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (fw_file == MAP_FAILED) {
        printf("FAIL: cannot map the firmware file\n");
        return 3;
    }
    fw_parse(fw_file, fw_size);
    pci_init();
    card_power_on(scenario == SC_RFKILL);

    volatile int rc = -999;
    deadline = 120000 * MS;
    armed = 1;
    if (!setjmp(escape))
        rc = iwlwifi_run();
    armed = 0;
    printf("  iwlwifi_run returned %d after %llu ms of card time\n", rc,
           (unsigned long long)(now_ns / MS));
    check("the driver finished the bring-up attempt (did not hang for "
          "120 s of card time)", !hung);

    switch (scenario) {
    case SC_OK: {
        scenario_ok(rc);
        uint32_t before = cs.pkts_delivered;
        volatile int polled_ok = 0;
        if (!hung && rc == IWL_OK) {
            fw_flood(300);
            deadline = now_ns + 10000 * MS;
            armed = 1;
            if (!setjmp(escape)) {
                for (int i = 0; i < 400; i++) {
                    model_advance(10 * MS);
                    iwlwifi_poll();
                }
                polled_ok = 1;
            }
            armed = 0;
        }
        scenario_ok_after(before, polled_ok);
        char why[64];
        check("the Device Manager: the card claimed by iwlwifi, working",
              device_said(why, sizeof(why)) == DEV_WORKING);
        break;
    }
    case SC_NOFW_ACK:
        scenario_nofw(rc);
        break;
    case SC_ASSERT: {
        scenario_assert(rc);
        char why[64];
        int state = device_said(why, sizeof(why));
        check("the Device Manager: the card failed, and why",
              state == DEV_FAILED && why[0]);
        break;
    }
    case SC_RFKILL: {
        scenario_rfkill(rc);
        char why[64];
        int state = device_said(why, sizeof(why));
        check("the Device Manager: the card works, its radio switch off",
              state == DEV_WORKING && strstr(why, "radio switch"));
        break;
    }
    case SC_SCAN:
        scenario_scan(rc);
        break;
    case SC_NOFILE:
        scenario_nofile(rc);
        break;
    }
    if (tasks)
        note("the bring-up started %d thread(s)", tasks);
    check("nothing the specifications forbid (every rule the model holds "
          "the driver to)", violation_count() == 0);
    printf("%d check(s) failed, %d rule(s) broken\n", failures,
           violation_count());
    return failures || violation_count() ? 1 : 0;
}
