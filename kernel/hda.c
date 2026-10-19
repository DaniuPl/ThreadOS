/* Intel HD Audio
 *
 * The sound controller every machine made after about 2005 has. It is
 * not a device one writes samples to, and that is the whole of what
 * makes it work to write: there are TWO things in the machine.
 *
 * The CONTROLLER sits on PCI and moves bytes out of memory by itself.
 * One hands it a list of buffers - a BDL - and tells it to go round
 * them; from then on it reads and the driver's only job is to keep
 * ahead of it.
 *
 * The CODEC sits behind the controller on a serial link, and it is a
 * graph. Not a register set: a set of numbered widgets - converters
 * that turn samples into a signal, pins that end at a socket or a
 * speaker, mixers between them - and the driver has to WALK it to find
 * out which converter reaches which pin. Two machines with the same
 * controller can have completely different graphs.
 *
 * Talking to the codec goes through two rings in memory: commands out
 * (CORB) and answers back (RIRB). Every question is a packet with the
 * codec's address, the node number, the verb and its payload, and the
 * answer comes back in its own time.
 *
 * We poll rather than take an interrupt, for the same reason as the
 * USB drivers here: a thread is polling anyway, and one road fewer is
 * one place fewer where something can go wrong.
 *
 * Everything the controller reads has to lie in physically contiguous
 * memory at an address it can use directly - the identity mapping,
 * where the virtual address equals the physical one.
 */

#include <stdint.h>
#include "include/hda.h"
#include "include/pci.h"
#include "include/pmm.h"
#include "include/printk.h"
#include "include/string.h"
#include "include/pit.h"
#include "include/config.h"
#include "include/task.h"

#ifdef __x86_64__
#include "../kernel64/paging64.h"
#define MAP_PHYS(a, n)   paging64_map_phys((a), (n))
#else
#include "include/paging.h"
#define MAP_PHYS(a, n)   paging_map_phys((a), (n))
#endif

/* --- the controller's registers ------------------------------------- */

#define REG_GCAP        0x00        /* 16: how many streams of each kind */
#define REG_GCTL        0x08        /* 32: bit 0 = out of reset */
#define REG_STATESTS    0x0E        /* 16: a bit per codec that answered */
#define REG_CORBLBASE   0x40
#define REG_CORBUBASE   0x44
#define REG_CORBWP      0x48        /* 16 */
#define REG_CORBRP      0x4A        /* 16 */
#define REG_CORBCTL     0x4C        /* 8 */
#define REG_CORBSIZE    0x4E        /* 8 */
#define REG_RIRBLBASE   0x50
#define REG_RIRBUBASE   0x54
#define REG_RIRBWP      0x58        /* 16 */
#define REG_RINTCNT     0x5A        /* 16 */
#define REG_RIRBCTL     0x5C        /* 8 */
#define REG_RIRBSTS     0x5D        /* 8 */
#define REG_RIRBSIZE    0x5E        /* 8 */

/* One output stream's registers, from its own base */
#define SD_CTL          0x00        /* 24 bits, so written as bytes */
#define SD_STS          0x03        /* 8 */
#define SD_LPIB         0x04        /* 32: where the controller is reading */
#define SD_CBL          0x08        /* 32: how much there is altogether */
#define SD_LVI          0x0C        /* 16: the last valid BDL entry */
#define SD_FMT          0x12        /* 16 */
#define SD_BDPL         0x18
#define SD_BDPU         0x1C

#define SDCTL_RUN       0x02
#define SDCTL_RESET     0x01

/* --- the codec's verbs ------------------------------------------------ */

/* Verbs come in two lengths and the length decides where they sit in
   the command word: a four-bit verb leaves sixteen bits of payload, a
   twelve-bit one leaves eight. Writing the short ones already shifted
   - 0x200 for verb two - is the mistake that makes a codec answer
   nothing, so they are written here as the numbers they are. */
#define VERB_GET_PARAM        0xF00
#define VERB_SET_CONV_FMT     0x002
#define VERB_SET_AMP          0x003
#define VERB_SET_CONV_STREAM  0x706
#define VERB_SET_PIN_CTL      0x707
#define VERB_SET_POWER        0x705

#define PARAM_VENDOR      0x00
#define PARAM_SUBNODES    0x04
#define PARAM_FUNC_TYPE   0x05
#define PARAM_WIDGET_CAP  0x09
#define PARAM_PIN_CAP     0x0C
#define PARAM_CONN_LIST   0x0E
#define PARAM_AMP_OUT_CAP 0x12

#define WIDGET_OUTPUT     0x0
#define WIDGET_PIN        0x4

/* 48 kHz, sixteen bits, two channels. The one format a codec has to
   support, so the one worth having. */
#define FORMAT_48K_16_2   0x0011
#define RATE              48000

#define CORB_ENTRIES      256
#define RIRB_ENTRIES      256
#define BDL_ENTRIES       4

static volatile uint8_t *reg;
static uint32_t out_base;             /* the first output stream's registers */
static int codec;                     /* which address answered */
static int dac, pin;                  /* the two nodes the sound goes through */
static int ready;
static int stream_active;
static int stream_owner = -1;
static uint32_t stream_fill;
static char name[48];

/* Where it stopped, when it stopped. A driver for a device that is
   there but silent is worth nothing without this: "no sound" can mean
   six different things and each has a different answer. */
static const char *why = "never looked for";

static int give_up(const char *reason)
{
    why = reason;
    return 0;
}

static volatile uint32_t *corb;
static volatile uint64_t *rirb;
static uint32_t corb_phys, rirb_phys;
static uint16_t corb_wp, rirb_rp;

static volatile uint8_t *bdl;
static uint32_t bdl_phys;
static volatile int16_t *buf;         /* the samples the controller reads */
static uint32_t buf_phys;

/* Loudness. Two widgets can have an amplifier - the converter and the
   pin - and which of them has a usable range differs between codecs,
   so both are moved together and the range is whichever has one. */
static int amp_node[2] = { -1, -1 };
static int amp_steps[2];
static int volume = 80;
static int muted;

/* Two pages of samples: at 48 kHz stereo that is about 43 ms, which is
   short enough to fill without hearing the seam and long enough that
   filling it is not a busy loop. */
#define BUF_PAGES 2
#define BUF_BYTES (BUF_PAGES * PAGE_SIZE)
#define BUF_FRAMES (BUF_BYTES / 4)

static uint32_t rd32(uint32_t off)  { return *(volatile uint32_t *)(reg + off); }
static uint16_t rd16(uint32_t off)  { return *(volatile uint16_t *)(reg + off); }
static void wr32(uint32_t off, uint32_t v) { *(volatile uint32_t *)(reg + off) = v; }
static void wr16(uint32_t off, uint16_t v) { *(volatile uint16_t *)(reg + off) = v; }
static void wr8(uint32_t off, uint8_t v)   { *(volatile uint8_t *)(reg + off) = v; }

/* Waiting here SLEEPS rather than spins, and that is not a nicety.
 *
 * A system call in long mode is entered with interrupts off - the
 * processor clears the flag on the way in. A driver that then spins
 * for a fifth of a second leaves the machine deaf for a fifth of a
 * second: no timer, and a PS/2 mouse that loses the middle of a packet
 * and never finds the framing again. Sleeping hands the core back to
 * the scheduler, which is where interrupts come back on.
 *
 * Before the scheduler exists there is nothing to sleep on, so the
 * spin stays as the fallback - and at that point in the boot nothing
 * else wants the machine anyway. */
static void wait_ms(uint32_t ms)
{
    if (ready) {
        task_sleep(ms);
        return;
    }
    uint32_t until = pit_ticks() + (ms + 9) / 10 + 1;
    while (pit_ticks() < until)
        __asm__ volatile("pause" ::: "memory");
}

/* --- asking the codec ------------------------------------------------
 *
 * A command goes into the ring and the write pointer is moved; the
 * answer turns up in the other ring in its own time. Waiting for it by
 * watching the write pointer is enough here - nothing else is asking.
 */
static uint32_t verb(int node, uint32_t v, uint32_t payload)
{
    uint32_t base = ((uint32_t)codec << 28) | ((uint32_t)node << 20);
    uint32_t cmd;

    if (v < 0x10)                      /* four-bit verb: sixteen of payload */
        cmd = base | (v << 16) | (payload & 0xFFFF);
    else                               /* twelve-bit verb: eight of payload */
        cmd = base | (v << 8) | (payload & 0xFF);

    corb_wp = (uint16_t)((corb_wp + 1) % CORB_ENTRIES);
    corb[corb_wp] = cmd;
    wr16(REG_CORBWP, corb_wp);

    for (int spin = 0; spin < 200000; spin++) {
        uint16_t wp = rd16(REG_RIRBWP) & 0xFF;
        if (wp != rirb_rp) {
            uint32_t answer;
            rirb_rp = (uint16_t)((rirb_rp + 1) % RIRB_ENTRIES);
            answer = (uint32_t)rirb[rirb_rp];
            /* And the thing without which exactly ONE question ever
               gets answered: the controller counts responses and
               stops running the command ring when the count reaches
               RINTCNT - which is one - until the status bit it raised
               is cleared. It is cleared by writing it back. */
            wr8(REG_RIRBSTS, 0x05);
            return answer;
        }
        __asm__ volatile("pause" ::: "memory");
    }
    return 0xFFFFFFFFu;                /* no answer - the caller decides */
}

static uint32_t param(int node, uint32_t which)
{
    return verb(node, VERB_GET_PARAM, which);
}

/* --- walking the graph -----------------------------------------------
 *
 * The root says how many function groups there are; a group says how
 * many widgets. We are looking for two of them: a converter that turns
 * samples into a signal, and a pin that ends somewhere a person can
 * hear. Taking the first of each is enough on every machine that has
 * one speaker, and on a machine with several it takes the one the
 * codec lists first - which is the one its maker put first.
 */
static int find_output(void)
{
    uint32_t sub = param(0, PARAM_SUBNODES);
    int first = (int)((sub >> 16) & 0xFF), count = (int)(sub & 0xFF);

    for (int fg = first; fg < first + count; fg++) {
        if ((param(fg, PARAM_FUNC_TYPE) & 0x7F) != 0x01)
            continue;                  /* not the audio one */

        uint32_t ws = param(fg, PARAM_SUBNODES);
        int wf = (int)((ws >> 16) & 0xFF), wc = (int)(ws & 0xFF);

        /* The function group has to be awake before anything below it
           answers with anything but zeros */
        verb(fg, VERB_SET_POWER, 0);
        wait_ms(10);

        dac = pin = -1;
        for (int w = wf; w < wf + wc; w++) {
            uint32_t cap = param(w, PARAM_WIDGET_CAP);
            int type = (int)((cap >> 20) & 0x0F);
            if (type == WIDGET_OUTPUT && dac < 0)
                dac = w;
            if (type == WIDGET_PIN && pin < 0) {
                /* An output pin, not a microphone socket: the pin's
                   capabilities say which way it goes. */
                if (param(w, PARAM_PIN_CAP) & (1u << 4))
                    pin = w;
            }
        }
        if (dac >= 0 && pin >= 0)
            return 1;
    }
    return 0;
}

/* --- the rings and the buffers --------------------------------------- */

static int rings_setup(void)
{
    corb_phys = pmm_alloc_frame();
    rirb_phys = pmm_alloc_frame();
    bdl_phys = pmm_alloc_frame();
    if (!corb_phys || !rirb_phys || !bdl_phys)
        return 0;
    corb = (volatile uint32_t *)(uintptr_t)corb_phys;
    rirb = (volatile uint64_t *)(uintptr_t)rirb_phys;
    bdl = (volatile uint8_t *)(uintptr_t)bdl_phys;
    memset((void *)corb, 0, PAGE_SIZE);
    memset((void *)rirb, 0, PAGE_SIZE);
    memset((void *)bdl, 0, PAGE_SIZE);

    /* The sample buffer: pages next to each other, because the
       controller reads it as one run of bytes and knows nothing about
       our page tables. The allocator hands out frames in order, so
       asking for two in a row either works at once or not at all -
       and saying so is better than quietly playing somebody else's
       memory. */
    uint32_t first = pmm_alloc_frame();
    if (!first)
        return 0;
    uint32_t prev = first;
    for (int i = 1; i < BUF_PAGES; i++) {
        uint32_t f = pmm_alloc_frame();
        if (!f || f != prev + PAGE_SIZE)
            return 0;
        prev = f;
    }
    buf_phys = first;
    buf = (volatile int16_t *)(uintptr_t)buf_phys;
    memset((void *)buf, 0, BUF_BYTES);

    wr8(REG_CORBCTL, 0);
    wr8(REG_RIRBCTL, 0);
    wr32(REG_CORBLBASE, corb_phys);
    wr32(REG_CORBUBASE, 0);
    wr32(REG_RIRBLBASE, rirb_phys);
    wr32(REG_RIRBUBASE, 0);
    wr8(REG_CORBSIZE, 0x02);           /* 256 entries */
    wr8(REG_RIRBSIZE, 0x02);

    /* The read pointer is reset by setting a bit and waiting for it to
       come back - the one place in this driver where the controller
       answers rather than obeys. */
    wr16(REG_CORBRP, 0x8000);
    for (int i = 0; i < 1000 && !(rd16(REG_CORBRP) & 0x8000); i++)
        ;
    wr16(REG_CORBRP, 0);
    wr16(REG_CORBWP, 0);
    corb_wp = 0;

    wr16(REG_RIRBWP, 0x8000);          /* the same, the other way */
    /* How many answers the controller collects before it decides we
       are not keeping up. It stops running the command ring at that
       count until the status bit is cleared - so with a count of one,
       exactly one question ever gets answered, and every one after it
       waits for an interrupt we do not take. We poll, so the count is
       set out of the way and the status is cleared after every answer
       anyway. */
    wr16(REG_RINTCNT, 0xFF);
    rirb_rp = 0;

    wr8(REG_CORBCTL, 0x02);            /* run */
    wr8(REG_RIRBCTL, 0x02);
    return 1;
}

/* The list of buffers the controller walks in a circle. Two halves of
   one run of memory, so that filling one while the other plays is a
   question of which half rather than of which allocation. */
static void bdl_setup(void)
{
    for (int i = 0; i < 2; i++) {
        volatile uint32_t *e = (volatile uint32_t *)(bdl + i * 16);
        e[0] = buf_phys + (uint32_t)i * (BUF_BYTES / 2);
        e[1] = 0;
        e[2] = BUF_BYTES / 2;
        e[3] = 0;                      /* no interrupt: we poll */
    }
}

static void stream_setup(void)
{
    wr8(out_base + SD_CTL, SDCTL_RESET);
    wait_ms(2);
    wr8(out_base + SD_CTL, 0);
    wait_ms(2);

    wr32(out_base + SD_BDPL, bdl_phys);
    wr32(out_base + SD_BDPU, 0);
    wr32(out_base + SD_CBL, BUF_BYTES);
    wr16(out_base + SD_LVI, 1);        /* two entries, so the last is one */
    wr16(out_base + SD_FMT, FORMAT_48K_16_2);
    /* Stream tag one, in the top nibble of the third byte of CTL */
    wr8(out_base + SD_CTL + 2, 0x10);
    wr8(out_base + SD_STS, 0x1C);      /* clear whatever was there */

    /* And the codec's end of the same stream */
    verb(dac, VERB_SET_CONV_FMT, FORMAT_48K_16_2);
    verb(dac, VERB_SET_CONV_STREAM, 0x10);   /* tag 1, channel 0 */
    verb(dac, VERB_SET_POWER, 0);
    verb(pin, VERB_SET_POWER, 0);
    verb(pin, VERB_SET_PIN_CTL, 0x40);       /* output enabled */

    /* What the amplifiers are, before anything is asked of them.
       Asking for more gain than a widget has is answered by ignoring
       the whole command, so the range comes from the widget itself:
       'numsteps' in its capabilities is its loudest. A widget with no
       amplifier says zero, and is then left alone. */
    for (int i = 0; i < 2; i++) {
        int node = i ? pin : dac;
        uint32_t cap = param(node, PARAM_AMP_OUT_CAP);
        /* The amplifier's capabilities, and the two fields in them are
           easy to swap: the NUMBER OF STEPS is at bits 14..8 and the
           SIZE of a step at 22..16, not the other way round. Reading
           them the wrong way round does not fail - it gives a mixer
           with three positions on a codec that has seventy-four, and
           the only sign of it is that quiet and loud sound the same. */
        int steps = (int)((cap >> 8) & 0x7F);
        amp_node[i] = node;
        amp_steps[i] = (cap == 0xFFFFFFFFu) ? 0 : steps;
    }
    /* How loud, from the settings - so it survives a restart like the
       background and the machine's name do. */
    volume = (int)config_get_num("volume", 80);
    hda_set_volume(volume);
}

/* --- coming up --------------------------------------------------------- */

int hda_init(void)
{
    struct pci_dev dev;

    if (ready)
        return 1;
    /* Class 0x04 (multimedia), subclass 0x03 (HD Audio). The
       programming interface is zero on every one of them. */
    if (!pci_find_class(0x04, 0x03, 0x00, &dev))
        return give_up("there is no HDA controller on the PCI bus");

    uint32_t bar = dev.bar[0] & 0xFFFFFFF0u;
    if (!bar || (dev.bar[0] & 0x01))
        return give_up("a controller with no registers in memory");
    pci_enable_bus_master(&dev);
    if (MAP_PHYS(bar, 0x4000) < 0)
        return give_up("I cannot map the registers");
    reg = (volatile uint8_t *)(uintptr_t)bar;

    /* Out of reset. The controller says when it is ready by reading
       the bit back, and nothing below works before it does. */
    wr32(REG_GCTL, 0);
    wait_ms(5);
    wr32(REG_GCTL, 1);
    for (int i = 0; i < 100 && !(rd32(REG_GCTL) & 1); i++)
        wait_ms(1);
    if (!(rd32(REG_GCTL) & 1))
        return give_up("the controller will not come out of reset");
    wait_ms(10);                       /* the codecs need a moment to answer */

    uint16_t states = rd16(REG_STATESTS);
    if (!states)
        return give_up("no codec answered");
    codec = 0;
    while (codec < 15 && !(states & (1u << codec)))
        codec++;

    /* Where the output streams begin: after the input ones, and how
       many of those there are is in GCAP. */
    uint16_t gcap = rd16(REG_GCAP);
    int in_streams = (gcap >> 8) & 0x0F;
    out_base = 0x80 + (uint32_t)in_streams * 0x20;

    if (!rings_setup())
        return give_up("no memory for the rings and the buffer");

    uint32_t vendor = param(0, PARAM_VENDOR);
    if (vendor == 0xFFFFFFFFu)
        return give_up("the codec does not answer questions");

    if (!find_output())
        return give_up("the codec has no output I know how to find");

    bdl_setup();
    stream_setup();

    /* The name is the codec's own identifier and the two nodes the
       sound goes through - which is what one wants to see on a machine
       where it does not work. */
    int n = 0;
    const char *hex = "0123456789abcdef";
    const char *p = "codec ";
    while (*p)
        name[n++] = *p++;
    for (int s = 28; s >= 0; s -= 4)
        name[n++] = hex[(vendor >> s) & 0xF];
    p = ", output ";
    while (*p)
        name[n++] = *p++;
    name[n++] = (char)('0' + (dac / 10) % 10);
    name[n++] = (char)('0' + dac % 10);
    p = " -> socket ";
    while (*p)
        name[n++] = *p++;
    name[n++] = (char)('0' + (pin / 10) % 10);
    name[n++] = (char)('0' + pin % 10);
    name[n] = '\0';

    ready = 1;
    return 1;
}

int hda_present(void)
{
    return ready;
}

const char *hda_name(void)
{
    return ready ? name : why;
}

/* --- playing ----------------------------------------------------------
 *
 * The controller walks the two halves of the buffer for ever; playing
 * is keeping the half it is NOT in filled with what comes next. When
 * the samples run out the rest is filled with silence, because a
 * buffer left as it was would be played again - a circle does not
 * stop by itself.
 */
int hda_play(const int16_t *samples, uint32_t frames)
{
    uint32_t half = BUF_FRAMES / 2;
    uint32_t done = 0;
    int filling, silent = 0;

    if (!ready || stream_active)
        return -1;

    /* Both halves before starting, so that nothing plays whatever was
       in the buffer before. Short of a whole buffer, the rest is
       silence - the samples are not repeated to fill it. */
    for (uint32_t i = 0; i < BUF_FRAMES; i++) {
        int16_t l = 0, r = 0;
        if (done < frames) {
            l = samples[done * 2];
            r = samples[done * 2 + 1];
            done++;
        }
        buf[i * 2] = l;
        buf[i * 2 + 1] = r;
    }

    wr8(out_base + SD_STS, 0x1C);
    wr8(out_base + SD_CTL, SDCTL_RUN);
    wr8(out_base + SD_CTL + 2, 0x10);          /* stream tag one */

    /* The controller walks the two halves for ever; playing is keeping
       the one it is NOT in filled with what comes next. When the
       samples run out the filling goes on with silence for two more
       halves - the whole buffer - because a circle does not stop by
       itself, and a half left as it was would be played a second
       time. That is the difference between a tone of the length asked
       for and a tone with a stutter on the end. */
    filling = 0;
    uint32_t guard = 0;
    while ((done < frames || silent < 2) && guard < 5000) {
        uint32_t pos = rd32(out_base + SD_LPIB);
        int playing = pos >= BUF_BYTES / 2;
        if (playing == filling) {
            /* Half a buffer is about twenty milliseconds, so a
               millisecond of sleep is fine grain and lets everything
               else on the machine keep running while we play. */
            guard++;
            task_sleep(1);
            continue;
        }
        guard = 0;
        if (done >= frames)
            silent++;
        for (uint32_t i = 0; i < half; i++) {
            uint32_t at = (uint32_t)filling * half + i;
            int16_t l = 0, r = 0;
            if (done < frames) {
                l = samples[done * 2];
                r = samples[done * 2 + 1];
                done++;
            }
            buf[at * 2] = l;
            buf[at * 2 + 1] = r;
        }
        filling ^= 1;
    }

    /* And let the silence through before stopping, so the last thing
       out of the speaker is nothing rather than a cut-off half. */
    wait_ms(BUF_FRAMES * 1000 / RATE + 5);
    wr8(out_base + SD_CTL, 0);
    return 0;
}

/* The codec reads the DMA ring while the caller prepares its next
   decoded frame. Wait until the half being filled is no longer in use;
   sleeping leaves the other programs responsive while audio plays. */
static int stream_wait_half(uint32_t filling)
{
    uint32_t guard = 0;
    while (guard < 5000) {
        if (task_killed())
            return -1;
        int playing = rd32(out_base + SD_LPIB) >= BUF_BYTES / 2;
        if ((uint32_t)playing != filling)
            return 0;
        guard++;
        task_sleep(1);
    }
    return -1;
}

int hda_stream_begin(void)
{
    if (!ready || stream_active)
        return -1;
    for (uint32_t i = 0; i < BUF_FRAMES * 2; i++)
        buf[i] = 0;
    stream_fill = 0;
    stream_owner = task_current_id();
    stream_active = 1;
    wr8(out_base + SD_STS, 0x1C);
    wr8(out_base + SD_CTL, SDCTL_RUN);
    wr8(out_base + SD_CTL + 2, 0x10);
    return 0;
}

int hda_stream_owned(void)
{
    return stream_active && stream_owner == task_current_id();
}

int hda_stream_write(const int16_t *samples, uint32_t frames)
{
    uint32_t half = BUF_FRAMES / 2;
    if (!stream_active || stream_owner != task_current_id() ||
        frames > half || (frames && !samples))
        return -1;
    if (stream_wait_half(stream_fill) < 0) {
        wr8(out_base + SD_CTL, 0);
        stream_active = 0;
        stream_owner = -1;
        return -1;
    }
    uint32_t at = stream_fill * half;
    for (uint32_t i = 0; i < half; i++) {
        buf[(at + i) * 2] = i < frames ? samples[i * 2] : 0;
        buf[(at + i) * 2 + 1] = i < frames ? samples[i * 2 + 1] : 0;
    }
    stream_fill ^= 1;
    return 0;
}

int hda_stream_end(void)
{
    if (!stream_active || stream_owner != task_current_id())
        return -1;
    /* The empty half follows the last samples, so the output stops on
       silence rather than cutting off the final decoded block. */
    if (hda_stream_write((const int16_t *)0, 0) < 0 ||
        stream_wait_half(stream_fill) < 0) {
        stream_active = 0;
        stream_owner = -1;
        wr8(out_base + SD_CTL, 0);
        return -1;
    }
    wr8(out_base + SD_CTL, 0);
    stream_active = 0;
    stream_owner = -1;
    return 0;
}

void hda_stream_abort(int task_id)
{
    if (stream_active && stream_owner == task_id) {
        wr8(out_base + SD_CTL, 0);
        stream_active = 0;
        stream_owner = -1;
    }
}

/* A square wave, because it is the shape a speaker makes loudest and
   the shape a test can recognise without a Fourier transform: the
   samples change sign exactly twice per period. */
int hda_beep(uint32_t hz, uint32_t ms)
{
    static int16_t tone[RATE / 4 * 2];      /* up to a quarter of a second */
    if (!ready || !hz)
        return -1;
    if (ms > 250)
        ms = 250;

    uint32_t frames = RATE * ms / 1000;
    uint32_t period = RATE / hz;
    if (!period)
        return -1;
    if (frames > sizeof(tone) / sizeof(tone[0]) / 2)
        frames = sizeof(tone) / sizeof(tone[0]) / 2;

    for (uint32_t i = 0; i < frames; i++) {
        int16_t v = (i % period) < period / 2 ? 8000 : -8000;
        tone[i * 2] = v;
        tone[i * 2 + 1] = v;
    }
    return hda_play(tone, frames);
}

/* --- how loud ----------------------------------------------------------
 *
 * The percentage is of the widget's own range, because that is the
 * only range there is. The steps are decibels, so thirty per cent is
 * far quieter than a third - which is how loudness works and not
 * something to correct.
 */
static void apply_volume(void)
{
    for (int i = 0; i < 2; i++) {
        int gain;
        if (amp_node[i] < 0 || !amp_steps[i])
            continue;
        gain = amp_steps[i] * volume / 100;
        if (gain > amp_steps[i])
            gain = amp_steps[i];
        /* bit 15 the output amp, 13 and 12 both channels, 7 mute */
        verb(amp_node[i], VERB_SET_AMP,
             0xB000 | (muted || !volume ? 0x80 : 0) | (uint32_t)(gain & 0x7F));
    }
}

void hda_set_volume(int percent)
{
    if (percent < 0)
        percent = 0;
    if (percent > 100)
        percent = 100;
    volume = percent;
    if (ready || amp_node[0] >= 0)
        apply_volume();
}

int hda_volume(void)
{
    return volume;
}

void hda_set_mute(int on)
{
    muted = on ? 1 : 0;
    if (ready)
        apply_volume();
}

int hda_muted(void)
{
    return muted;
}

int hda_volume_steps(void)
{
    int most = 0;
    for (int i = 0; i < 2; i++)
        if (amp_steps[i] > most)
            most = amp_steps[i];
    return most;
}

/* Written down the way the background and the machine's name are.
 * Loudness is a knob and not a change to the system, which is why
 * nothing here asks who is turning it - and why it is the one setting
 * a program in ring 3 may write.
 */
int hda_remember(void)
{
    char text[8];
    int n = 0, v = volume;

    do {
        text[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v && n < 4);
    for (int i = 0; i < n / 2; i++) {
        char t = text[i];
        text[i] = text[n - 1 - i];
        text[n - 1 - i] = t;
    }
    text[n] = '\0';
    return config_set("volume", text);
}
