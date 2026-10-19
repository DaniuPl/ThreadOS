/* gmon - how much of the machine is in use (ring 3)
 *
 * Twice a second it asks the kernel for its figures (sys_sysinfo) and
 * shows them: a capsule filled as far as the memory is used, the
 * numbers behind it, how long the machine has been up - and a strip of
 * the last minute, because "how much" means little without "and is it
 * growing".
 *
 * Drawn the way the desktop draws, in the theme's colours, asked for
 * every time so that a switch of the theme is followed at once. One
 * program for both kernels (uapi.h).
 */

#include "udraw.h"
#include "ubytes.h"

#define W 220                 /* the content */
#define H 132
#define PAD 12
#define HIST 60               /* half-second samples: the last thirty seconds */

#ifdef __x86_64__
typedef struct sysinfo64 info_t;
#else
typedef struct sysinfo info_t;
#endif

static int history[HIST];     /* percent used, oldest first */
static int samples;

/* A number with a thin space every three digits: 130 560 reads, 130560
   has to be counted */
static void num(char *d, uint32_t v)
{
    char tmp[16];
    int n = 0, k = 0;
    do {
        if (k && k % 3 == 0)
            tmp[n++] = ' ';
        tmp[n++] = (char)('0' + v % 10);
        v /= 10;
        k++;
    } while (v);
    int j = 0;
    while (n)
        d[j++] = tmp[--n];
    d[j] = '\0';
}

static void cat(char *d, const char *s)
{
    while (*d)
        d++;
    while (*s)
        *d++ = *s++;
    *d = '\0';
}

static void row(int w, const struct ud_look *l, int y, const char *label,
                const char *value)
{
    int base = ud_baseline(y, 18, UD_PX);
    ud_text(w, PAD, base, label, l->dim, UD_SANS, UD_PX);
    ud_text_right(w, W - PAD, base, value, l->text, UD_SANS, UD_PX);
}

void _start(void)
{
    int w = (int)win_create(W, H, "Monitor");
    if (w < 0)
        sys_exit(1);

    for (;;) {
        struct win_event ev;
        while (win_poll(w, &ev) == 1)
            if (ev.type == WEV_CLOSE)
                sys_exit(0);

        info_t si;
        sys_sysinfo(&si);
        struct ud_look l;
        ud_look(&l);

        int pct = si.ram_kib ? (int)(si.used_kib * 100 / si.ram_kib)
                             : 0;
        if (samples < HIST) {
            history[samples++] = pct;
        } else {
            for (int i = 1; i < HIST; i++)
                history[i - 1] = history[i];
            history[HIST - 1] = pct;
        }

        ud_fill(w, 0, 0, W, H, l.bg);

        /* The heading, and the share in the accent at the other end */
        char p[8];
        num(p, (uint32_t)pct);
        cat(p, "%");
        ud_text(w, PAD, ud_baseline(6, 18, 13), "Memory", l.text, UD_BOLD, 13);
        ud_text_right(w, W - PAD, ud_baseline(6, 18, 13), p, l.accent,
                      UD_BOLD, 13);

        /* The capsule: the track, and as much of it filled as is used */
        int bw = W - 2 * PAD;
        int fill = si.ram_kib ? (int)(si.used_kib * (uint32_t)bw /
                                      si.ram_kib) : 0;
        if (fill < 8 && si.used_kib)
            fill = 8;
        ud_round(w, PAD, 30, bw, 8, 4, l.row);
        if (fill > 0)
            ud_round(w, PAD, 30, fill, 8, 4, l.accent);

        char v[32];
        format_bytes((uint64_t)si.used_kib * 1024, v, sizeof(v));
        row(w, &l, 44, "Used", v);
        format_bytes((uint64_t)si.ram_kib * 1024, v, sizeof(v));
        row(w, &l, 62, "Total", v);
        num(v, si.uptime_s);
        cat(v, " s");
        row(w, &l, 80, "Up for", v);

        /* The last half minute, as a line: flat is good news */
        int gy = 104, gh = 20;
        ud_round(w, PAD, gy, bw, gh, 4, l.field);
        for (int i = 1; i < samples; i++) {
            int x0 = PAD + 2 + (i - 1) * (bw - 4) / (HIST - 1);
            int x1 = PAD + 2 + i * (bw - 4) / (HIST - 1);
            int y0 = gy + gh - 3 - history[i - 1] * (gh - 6) / 100;
            int y1 = gy + gh - 3 - history[i] * (gh - 6) / 100;
            ud_line(w, x0, y0, x1, y1, 20, l.accent);
        }

        sys_sleep(500);
    }
}
