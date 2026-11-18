/* The phone shell's own sound: the notification chime and the Recorder
 *
 * Both go through the speaker and microphone of vsnd.c, as a program's
 * sound does, with the shell as their owner (0 - no program's pid). A
 * thread of their own each does the waiting - a buffer at a time, at
 * the pace the device takes them - so the shell's drawing never stands
 * still for sound:
 *
 *   "chime" plays the notification sound and the Recorder's recordings
 *   "recorder" records, into pages of its own: a recording is kept in
 *   memory, at the stereo 16-bit 48 kHz the device gives
 *
 * The chime is two notes, A5 and E6, each fading out: no floating
 * point in the kernel, so the sine is a table of its first quarter and
 * the notes a phase that steps through it.
 */

#include <stdint.h>
#include "mobapps.h"
#include "thread.h"
#include "vsnd.h"
#include "../kernel/include/gfx.h"
#include "../kernel/include/pmm.h"
#include "../kernel/include/printk.h"
#include "../kernel/include/string.h"
#include "../kernel/include/uitext.h"
#include "../kernel/include/video.h"

#define SHELL          0        /* the speaker's and microphone's owner */
#define RATE           48000
#define CHUNK          1200     /* frames handed over at a time: 25 ms */

/* The Recorder's screen, logical pixels */
#define REC_TIME_Y     190      /* the time's baseline */
#define REC_LIST_Y     236      /* the first recording's row */
#define REC_ROW_H      52
#define REC_ROWS       5
#define REC_BUTTON_UP  120      /* the button's middle, from the bottom */
#define REC_BUTTON_D   72
#define REC_MAX_S      60       /* a recording's longest */
#define REC_KEPT       8

#define RED            0xFF3B30

/* --- the chime ------------------------------------------------------------ */

static const int16_t quarter[65] = {
    0, 804, 1608, 2410, 3212, 4011, 4808, 5602,
    6393, 7179, 7962, 8739, 9512, 10278, 11039, 11793,
    12539, 13279, 14010, 14732, 15446, 16151, 16846, 17530,
    18204, 18868, 19519, 20159, 20787, 21403, 22005, 22594,
    23170, 23731, 24279, 24811, 25329, 25832, 26319, 26790,
    27245, 27683, 28105, 28510, 28898, 29268, 29621, 29956,
    30273, 30571, 30852, 31113, 31356, 31580, 31785, 31971,
    32137, 32285, 32412, 32521, 32609, 32678, 32728, 32757,
    32767,
};

/* sin of a phase (the whole turn is 2^32), 32767 at its peak */
static int sine(uint32_t phase)
{
    uint32_t pos = (phase >> 14) & 0xFFFF;  /* within its quarter */
    if (phase & 0x40000000u)
        pos = 0x10000 - pos;                /* the falling quarters */
    uint32_t i = pos >> 10, frac = pos & 0x3FF;
    int v = i < 64 ? quarter[i] +
                     (((quarter[i + 1] - quarter[i]) * (int)frac) >> 10) :
                     quarter[64];
    return phase & 0x80000000u ? -v : v;
}

#define CHIME_PEAK     9000
static const struct note { uint32_t hz, ms; } chime[] = {
    { 880, 110 }, { 1319, 190 },
};

/* --- what the threads are asked ------------------------------------------- */

struct recording {
    int16_t *pcm;
    uint32_t pages;             /* allocated */
    volatile uint32_t frames;
    int number;
};

static struct recording kept[REC_KEPT];     /* newest first */
static int kept_count, numbered;
static struct recording taking;             /* being recorded */
/* What the threads are asked, and what they say back, goes between
   cores: a request's details are stored before the bit that asks
   (release), and read after it is taken (acquire) - plain stores may be
   seen in another order on another core, and a thread woken by the
   bit would find nothing to do and sleep through its work */
#define WORK_CHIME     1
#define WORK_PLAY      2
#define WORK_RECORD    4
static volatile int rec_on, rec_stop;       /* recording; asked to end */
static volatile int play_asked, play_stop;
static volatile int playing_number;         /* 0: nothing */
static volatile int sound_seq;              /* what the screen shows moved */
static volatile int chime_work, rec_work;   /* WORK_* bits */
static struct thread *chime_thread, *rec_thread;

/* Frames to the speaker, CHUNK at a time; 0 once stopped */
static int play_frames(const int16_t *pcm, uint32_t n)
{
    for (uint32_t done = 0; done < n;) {
        if (play_stop)
            return 0;
        uint32_t m = n - done < CHUNK ? n - done : CHUNK;
        if (vsnd_write(pcm + done * 2, m) <= 0)
            return 0;
        done += m;
    }
    return 1;
}

static void play_chime(void)
{
    static int16_t buf[CHUNK * 2];
    int r = vsnd_begin(SHELL);
    if (r < 0) {
        /* a program plays: no chime over it */
        printk("[sound] no chime: %s\n", r == -2 ? "the speaker is busy" :
               "the speaker would not start");
        return;
    }
    for (unsigned k = 0; k < sizeof(chime) / sizeof(chime[0]); k++) {
        uint32_t step = (uint32_t)(((uint64_t)chime[k].hz << 32) / RATE);
        uint32_t len = chime[k].ms * RATE / 1000, phase = 0;
        for (uint32_t done = 0; done < len;) {
            uint32_t m = len - done < CHUNK ? len - done : CHUNK;
            for (uint32_t i = 0; i < m; i++) {
                /* fading out over the note */
                int v = sine(phase) * CHIME_PEAK / 32767 *
                        (int)(len - done - i) / (int)len;
                buf[i * 2] = buf[i * 2 + 1] = (int16_t)v;
                phase += step;
            }
            if (vsnd_write(buf, m) <= 0)
                break;
            done += m;
        }
    }
    vsnd_end();
    printk("[sound] chime played\n");
}

static void play_recording(int number)
{
    struct recording *r = 0;
    for (int i = 0; i < kept_count; i++)
        if (kept[i].number == number)
            r = &kept[i];
    if (!r)
        return;
    if (vsnd_begin(SHELL) < 0) {
        printk("[recorder] the speaker is busy\n");
        return;
    }
    playing_number = number;
    sound_seq++;
    printk("[recorder] playing recording %d (%u frames)\n", number,
           r->frames);
    int whole = play_frames(r->pcm, r->frames);
    vsnd_end();
    printk("[recorder] recording %d %s\n", number,
           whole ? "played" : "stopped");
    playing_number = 0;
    sound_seq++;
}

static void chime_main(void *arg)
{
    (void)arg;
    for (;;) {
        thread_wait(&chime_work, ~0ULL);
        int work = __atomic_exchange_n(&chime_work, 0, __ATOMIC_ACQ_REL);
        if (work & WORK_CHIME)
            play_chime();
        if (work & WORK_PLAY) {
            play_stop = 0;
            play_recording(play_asked);
        }
    }
}

static void rec_main(void *arg)
{
    (void)arg;
    for (;;) {
        thread_wait(&rec_work, ~0ULL);
        if (!(__atomic_exchange_n(&rec_work, 0, __ATOMIC_ACQ_REL) &
              WORK_RECORD))
            continue;
        struct recording *r = &taking;
        uint32_t room = r->pages * PAGE_SIZE / 4;
        while (!rec_stop && r->frames < room) {
            uint32_t want = room - r->frames < CHUNK * 4 ?
                            room - r->frames : CHUNK * 4;
            int got = vsnd_mic_read(r->pcm + r->frames * 2, want);
            if (got <= 0)
                break;
            uint32_t was = r->frames / RATE;
            r->frames += (uint32_t)got;
            if (r->frames / RATE != was)
                sound_seq++;    /* the time on the screen moves on */
        }
        vsnd_mic_stop();
        /* the pages past its end go back */
        uint32_t used = (r->frames * 4 + PAGE_SIZE - 1) / PAGE_SIZE;
        uintptr_t base = (uintptr_t)r->pcm;
        for (uint32_t p = used ? used : 1; p < r->pages; p++)
            pmm_free_frame((uint32_t)(base + (uintptr_t)p * PAGE_SIZE));
        r->pages = used ? used : 1;
        __atomic_store_n(&rec_on, 0, __ATOMIC_RELEASE);
        sound_seq++;
    }
}

void mobsound_init(void)
{
    chime_thread = thread_create("chime", chime_main, 0);
    rec_thread = thread_create("recorder", rec_main, 0);
}

void mob_chime(void)
{
    if (!vsnd_present() || !chime_thread || mob_prefs.silent ||
        mob_prefs.dnd)
        return;
    __atomic_or_fetch(&chime_work, WORK_CHIME, __ATOMIC_RELEASE);
    thread_wake(chime_thread);
}

int rec_changes(void)
{
    return sound_seq;
}

/* --- the Recorder ------------------------------------------------------- */

static void forget(struct recording *r)
{
    uintptr_t base = (uintptr_t)r->pcm;
    for (uint32_t p = 0; p < r->pages; p++)
        pmm_free_frame((uint32_t)(base + (uintptr_t)p * PAGE_SIZE));
    r->pcm = 0;
}

/* Recording ends: the thread lets the microphone go, and the recording
   joins the list, newest first */
static void rec_end(void)
{
    rec_stop = 1;
    while (__atomic_load_n(&rec_on, __ATOMIC_ACQUIRE))
        thread_sleep_ms(5);
    if (kept_count == REC_KEPT)
        forget(&kept[--kept_count]);
    for (int i = kept_count; i > 0; i--)
        kept[i] = kept[i - 1];
    kept[0] = taking;
    kept_count++;
    taking.pcm = 0;
    printk("[recorder] recording %d: %u frames\n", kept[0].number,
           kept[0].frames);
}

static void rec_begin(void)
{
    uint32_t pages = (uint32_t)(REC_MAX_S * RATE * 4 / PAGE_SIZE);
    uint32_t at = pmm_alloc_contiguous(pages);
    if (!at) {
        printk("[recorder] no memory for a recording\n");
        return;
    }
    if (vsnd_mic_start(SHELL) < 0) {
        printk("[recorder] the microphone is %s\n",
               vsnd_mic_present() ? "busy" : "not there");
        taking.pcm = (int16_t *)(uintptr_t)at;
        taking.pages = pages;
        forget(&taking);
        return;
    }
    taking.pcm = (int16_t *)(uintptr_t)at;
    taking.pages = pages;
    taking.frames = 0;
    taking.number = ++numbered;
    rec_stop = 0;
    rec_on = 1;
    __atomic_or_fetch(&rec_work, WORK_RECORD, __ATOMIC_RELEASE);
    thread_wake(rec_thread);
    printk("[recorder] recording %d begins\n", taking.number);
}

static void duration(char *s, uint32_t frames)
{
    uint32_t sec = frames / RATE;
    char *p = mob_put_num(s, sec / 60);
    *p++ = ':';
    *p++ = (char)('0' + sec % 60 / 10);
    *p++ = (char)('0' + sec % 10);
    *p = 0;
}

/* A play triangle or a stop square, white, in a disc */
static void row_icon(int cx, int cy, int playing)
{
    gfx_disc16(cx * 16, cy * 16, 14 * 16, mob_accent(), 255);
    if (playing) {
        gfx_fill(cx - 5, cy - 5, 10, 10, 0xFFFFFF, 255);
        return;
    }
    for (int r = -7; r <= 7; r++) {
        int w = (7 - (r < 0 ? -r : r)) * 11 / 7;
        gfx_fill(cx - 4, cy + r, w + 1, 1, 0xFFFFFF, 255);
    }
}

void recorder_draw(void)
{
    struct mob_theme t;
    mob_theme(&t);
    int lw = video_w(), lh = video_h();
    gfx_fill(0, 0, lw, lh, t.bg, 255);
    mob_status_bar(t.fg);
    mob_title("Recorder", t.fg);

    /* the time recorded, counting up - waiting at 0:00 */
    char s[24];
    duration(s, rec_on ? taking.frames : 0);
    mob_centred(UI_BOLD, 56, lw / 2, REC_TIME_Y, s, rec_on ? t.fg : t.muted);
    if (!vsnd_mic_present())
        mob_centred(UI_SANS, 16, lw / 2, REC_LIST_Y + 24, "No microphone",
                    t.muted);
    else if (!kept_count && !rec_on)
        mob_centred(UI_SANS, 16, lw / 2, REC_LIST_Y + 24,
                    "Tap the button to record", t.muted);
    for (int i = 0; i < kept_count && i < REC_ROWS; i++) {
        int y = REC_LIST_Y + i * REC_ROW_H;
        gfx_rrect(16, y, lw - 32, REC_ROW_H - 8, 12, t.card, 255);
        int mid = y + (REC_ROW_H - 8) / 2;
        row_icon(44, mid, playing_number == kept[i].number);
        char *p = mob_put(s, "Recording ");
        mob_put_num(p, (uint32_t)kept[i].number);
        ui_font_text(UI_BOLD, 16, 70, mid + 6, s, t.fg);
        duration(s, kept[i].frames);
        ui_font_text(UI_SANS, 15, lw - 32 - ui_font_width(UI_SANS, 15, s),
                     mid + 6, s, t.muted);
    }
    /* the button: a ring, and in it a red disc - a square, recording */
    int cx = lw / 2, cy = lh - REC_BUTTON_UP, d = REC_BUTTON_D;
    gfx_circle16(cx * 16, cy * 16, d * 8, 4 * 16, t.fg, 255);
    if (rec_on)
        gfx_rrect(cx - d / 5, cy - d / 5, 2 * (d / 5), 2 * (d / 5), 6, RED,
                  255);
    else
        gfx_disc16(cx * 16, cy * 16, (d / 2 - 8) * 16, RED, 255);
    mob_pill(t.fg);
}

int recorder_tap(int x, int y)
{
    int lw = video_w(), lh = video_h();
    int dx = x - lw / 2, dy = y - (lh - REC_BUTTON_UP);
    if (dx * dx + dy * dy <= REC_BUTTON_D * REC_BUTTON_D / 4) {
        if (playing_number) {
            /* what plays stops for what is to be recorded */
            play_stop = 1;
            while (playing_number)
                thread_sleep_ms(5);
        }
        if (rec_on)
            rec_end();
        else if (vsnd_mic_present())
            rec_begin();
        return 1;
    }
    if (rec_on || y < REC_LIST_Y)
        return 0;
    int i = (y - REC_LIST_Y) / REC_ROW_H;
    if (i >= kept_count || i >= REC_ROWS ||
        y >= REC_LIST_Y + i * REC_ROW_H + REC_ROW_H - 8)
        return 0;
    if (playing_number == kept[i].number) {
        play_stop = 1;          /* a second tap stops it */
    } else if (!playing_number) {
        play_asked = kept[i].number;
        __atomic_or_fetch(&chime_work, WORK_PLAY, __ATOMIC_RELEASE);
        thread_wake(chime_thread);
    }
    return 1;
}
