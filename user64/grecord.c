/* grecord - the Recorder: the microphone into a file
 *
 * A round red button records, and the same button, square now, stops;
 * the recording is kept in the Recordings folder of the home as a WAV
 * file - 48 kHz, 16 bits, two channels, exactly what came in - and
 * Play plays the last one back. While it records, a clock counts and a
 * bar shows how loud it is, and the desktop's bar has an orange dot:
 * the kernel draws that from the microphone's state, not from this.
 *
 * The samples come from SYS64_MIC a few dozen milliseconds at a time;
 * the driver keeps about two thirds of a second, so the loop comes back
 * well within that. Built against musl, for memory that grows.
 */

#include "uapi.h"
#include "udraw.h"
#include "ulocale.h"
#include "umic.h"

#include <stdlib.h>
#include <string.h>

#define W0 360
#define H0 260
#define TIME_Y 70             /* the clock's baseline */
#define METER_Y 92            /* the loudness bar */
#define METER_W 240
#define METER_H 8
#define BUTTON_D 60           /* the record button */
#define BUTTON_Y 118
#define PLAY_W 70             /* Play, at the bottom right */
#define PLAY_H 26
#define MAX_SECONDS 600

static int win = -1;
static int cw = W0, ch = H0;
static struct ud_look l;
static struct mic_info info;
static int recording;
static int16_t *rec;               /* stereo frames */
static uint32_t rec_frames, rec_cap;
static uint64_t started_ms;
static int level;                  /* 0..255, the last chunk's peak */
static char status[80];
static char last[200];             /* the last recording's path */

static uint64_t now_ms(void)
{
    return sys_ticks() * 10;           /* the timer ticks a hundred a second */
}

static void say(const char *a, const char *b)
{
    strncpy(status, a, sizeof(status) - 1);
    status[sizeof(status) - 1] = 0;
    if (b)
        strncat(status, b, sizeof(status) - strlen(status) - 1);
}

static void draw(void);

/* --- recording -------------------------------------------------------------- */

static void take(void)
{
    static int16_t chunk[4096 * 2];
    for (;;) {
        int n = sys_mic(MIC_READ, (int64_t)(uintptr_t)chunk, 4096);
        if (n <= 0)
            return;
        if (rec_frames + (uint32_t)n > rec_cap) {
            uint32_t want = rec_cap ? rec_cap * 2 : 48000 * 10;
            if (want > 48000u * MAX_SECONDS)
                want = 48000u * MAX_SECONDS;
            if (rec_frames + (uint32_t)n > want)
                return;                /* full: the rest is not kept */
            int16_t *grown = realloc(rec, (size_t)want * 4);
            if (!grown)
                return;
            rec = grown;
            rec_cap = want;
        }
        memcpy(rec + rec_frames * 2, chunk, (size_t)n * 4);
        rec_frames += (uint32_t)n;
        int peak = 0;
        for (int i = 0; i < n * 2; i++) {
            int v = chunk[i] < 0 ? -chunk[i] : chunk[i];
            if (v > peak)
                peak = v;
        }
        level = peak * 255 / 32768;
    }
}

static void put16(uint8_t *o, uint32_t v)
{
    o[0] = (uint8_t)v;
    o[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *o, uint32_t v)
{
    put16(o, v);
    put16(o + 2, v >> 16);
}

static int write_all(int fd, const void *p, uint32_t n)
{
    const uint8_t *b = p;
    while (n) {
        int64_t w = sys_write(fd, b, n);
        if (w <= 0)
            return -1;
        b += w;
        n -= (uint32_t)w;
    }
    return 0;
}

/* Into the Recordings folder, under the first number not taken */
static void save(void)
{
    char path[200];
    if (sys_getcwd(path, sizeof(path) - 40) < 0)
        strcpy(path, "/");
    size_t n = strlen(path);
    if (n && path[n - 1] != '/')
        path[n++] = '/';
    strcpy(path + n, "Recordings");
    sys_mkdir(path);
    n = strlen(path);
    path[n++] = '/';
    for (int k = 1; k < 10000; k++) {
        char *p = path + n;
        strcpy(p, "Recording-");
        p += 10;
        char d[8];
        int dn = 0, v = k;
        do {
            d[dn++] = (char)('0' + v % 10);
            v /= 10;
        } while (v);
        while (dn)
            *p++ = d[--dn];
        strcpy(p, ".wav");
        int fd = (int)sys_open(path, O_READ);
        if (fd >= 0) {
            sys_close(fd);
            continue;
        }
        fd = (int)sys_open(path, O_WRITE | O_CREATE | O_TRUNC);
        if (fd < 0)
            break;
        uint8_t h[44];
        memcpy(h, "RIFF", 4);
        put32(h + 4, 36 + rec_frames * 4);
        memcpy(h + 8, "WAVEfmt ", 8);
        put32(h + 16, 16);
        put16(h + 20, 1);
        put16(h + 22, 2);
        put32(h + 24, 48000);
        put32(h + 28, 48000 * 4);
        put16(h + 32, 4);
        put16(h + 34, 16);
        memcpy(h + 36, "data", 4);
        put32(h + 40, rec_frames * 4);
        int ok = write_all(fd, h, 44) == 0 &&
                 write_all(fd, rec, rec_frames * 4) == 0;
        sys_close(fd);
        if (ok) {
            strcpy(last, path);
            say("Saved ", path + n);
        } else {
            say("Could not save ", path + n);
        }
        return;
    }
    say("Could not save the recording", 0);
}

static void toggle(void)
{
    if (!recording) {
        int r = sys_mic(MIC_OPEN, 0, 0);
        if (r < 0) {
            say(r == -2 ? "Another program is using the microphone."
                        : "There is no microphone.", 0);
            return;
        }
        recording = 1;
        rec_frames = 0;
        level = 0;
        started_ms = now_ms();
        say("Recording...", 0);
    } else {
        take();
        sys_mic(MIC_CLOSE, 0, 0);
        recording = 0;
        level = 0;
        /* Writing a minute of sound takes a while: said first, so the
           window does not look stuck */
        say("Saving...", 0);
        draw();
        save();
    }
}

/* The last recording, played back - the window waits while it plays */
static void play(void)
{
    if (recording || !rec_frames)
        return;
    say("Playing...", 0);
    if (sys_snd_stream(SND_STREAM_BEGIN, 0, 0) != 0) {
        say("The sound is busy.", 0);
        return;
    }
    for (uint32_t at = 0; at < rec_frames; at += SND_STREAM_MAX_FRAMES) {
        uint32_t count = rec_frames - at;
        if (count > SND_STREAM_MAX_FRAMES)
            count = SND_STREAM_MAX_FRAMES;
        if (sys_snd_stream(SND_STREAM_WRITE, rec + at * 2, count) < 0)
            break;
    }
    sys_snd_stream(SND_STREAM_END, 0, 0);
    say("", 0);
}

/* --- drawing ---------------------------------------------------------------- */

static void draw(void)
{
    ud_look(&l);
    ud_fill(win, 0, 0, cw, ch, l.bg);
    uint32_t secs = recording ? (uint32_t)((now_ms() - started_ms) / 1000)
                              : rec_frames / 48000;
    char t[8];
    t[0] = (char)('0' + secs / 600 % 10);
    t[1] = (char)('0' + secs / 60 % 10);
    t[2] = ':';
    t[3] = (char)('0' + secs % 60 / 10);
    t[4] = (char)('0' + secs % 10);
    t[5] = 0;
    int tw = ud_width(t, UD_BOLD, 28);
    ud_text(win, (cw - tw) / 2, TIME_Y, t, recording ? l.danger : l.text,
            UD_BOLD, 28);
    int mx = (cw - METER_W) / 2;
    ud_round(win, mx, METER_Y, METER_W, METER_H, METER_H / 2, l.row);
    if (recording && level > 0)
        ud_round(win, mx, METER_Y, METER_W * level / 255 < METER_H ? METER_H :
                 METER_W * level / 255, METER_H, METER_H / 2, l.good);
    /* the button: a red disc in a ring, a red square while recording */
    int bx = (cw - BUTTON_D) / 2;
    ud_round(win, bx, BUTTON_Y, BUTTON_D, BUTTON_D, BUTTON_D / 2, l.text);
    ud_round(win, bx + 3, BUTTON_Y + 3, BUTTON_D - 6, BUTTON_D - 6,
             BUTTON_D / 2 - 3, l.bg);
    if (recording)
        ud_round(win, bx + 18, BUTTON_Y + 18, BUTTON_D - 36, BUTTON_D - 36, 5,
                 0xFF3B30);
    else
        ud_round(win, bx + 7, BUTTON_Y + 7, BUTTON_D - 14, BUTTON_D - 14,
                 (BUTTON_D - 14) / 2, 0xFF3B30);
    if (!recording && rec_frames) {
        ud_round(win, cw - PLAY_W - 12, ch - PLAY_H - 10, PLAY_W, PLAY_H, 8,
                 l.row);
        ud_text_mid_l(win, cw - PLAY_W - 12, ch - PLAY_H - 10, PLAY_W, PLAY_H,
                      "Play", l.text, UD_SANS, UD_PX);
    }
    if (status[0])
        ud_text_l(win, 12, ch - 18, status, l.dim, UD_SANS, 11);
    else if (!info.present)
        ud_text_l(win, 12, ch - 18, "There is no microphone.", l.dim,
                  UD_SANS, 11);
}

static void click(int x, int y)
{
    int bx = (cw - BUTTON_D) / 2;
    if (x >= bx && x < bx + BUTTON_D && y >= BUTTON_Y &&
        y < BUTTON_Y + BUTTON_D) {
        toggle();
        return;
    }
    if (x >= cw - PLAY_W - 12 && x < cw - 12 && y >= ch - PLAY_H - 10 &&
        y < ch - 10)
        play();
}

int main(void)
{
    win = (int)win_create(W0, H0, "Recorder");
    if (win < 0)
        return 1;
    ulocale_window_title(win, "Recorder", "Dyktafon");
    sys_mic(MIC_INFO, (int64_t)(uintptr_t)&info, 0);
    draw();
    uint32_t last_secs = 0;
    for (;;) {
        struct win_event ev;
        int again = 0;
        while (win_poll(win, &ev) == 1) {
            if (ev.type == WEV_CLOSE) {
                if (recording) {
                    take();
                    sys_mic(MIC_CLOSE, 0, 0);
                    save();
                }
                return 0;
            }
            if (ev.type == WEV_SIZE) {
                cw = ev.x;
                ch = ev.y;
            } else if (ev.type == WEV_CLICK) {
                click(ev.x, ev.y);
            } else if (ev.type == WEV_KEY && ev.val == ' ') {
                toggle();
            }
            again = 1;
        }
        if (recording) {
            take();
            uint32_t secs = (uint32_t)((now_ms() - started_ms) / 1000);
            if (secs != last_secs)
                again = 1;
            last_secs = secs;
            again = 1;                 /* the meter moves */
        }
        if (again)
            draw();
        sys_sleep(40);
    }
}
