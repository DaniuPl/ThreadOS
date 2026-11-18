/* ThreadOS mobile: Notes, Calculator and Settings
 *
 * The phone's own apps beyond the first three, drawn on the whole
 * screen by the shell (mobile.c), in the look Settings chooses:
 *
 *   Notes: a page of text and a keyboard on the screen below it - a
 *   phone has no other - with a layer of digits and signs and one of
 *   Polish letters; the text stays while the phone is on (there is no
 *   disk to keep it on yet)
 *
 *   Calculator: what a phone's calculator does, in decimal fixed point
 *   - six places after the point, twelve digits before - since the
 *   kernel is built without the FP/SIMD registers; each result said on
 *   the serial port
 *
 *   Settings: the network and airplane mode (which switches the card
 *   off), the dark look, do not disturb, the screen's brightness, the
 *   battery, and About
 *
 * Every size here is a plain number of logical pixels: the tests read
 * them from this file. What sits at the bottom of the screen is placed
 * from its bottom, and what spans it from its width: the display size
 * (Settings) changes both.
 */

#include <stdint.h>
#include "mobapps.h"
#include "vsnd.h"
#include "../kernel/include/gfx.h"
#include "../kernel/include/printk.h"
#include "../kernel/include/string.h"
#include "../kernel/include/uitext.h"
#include "../kernel/include/video.h"

/* The screen's logical size: it changes with the display size */
#define MOB_LW         video_w()
#define MOB_LH         video_h()
#define APP_TITLE_Y    68
#define ORANGE         0xFF9F0A

struct mob_prefs mob_prefs = {
    .dark = 1, .brightness = 100, .volume = 70, .banners = 1,
    .lock_notes = 1, .show_pct = 1, .clock24 = 1,
};

static const uint32_t accents[MOB_ACCENTS] = {
    0x0A84FF, 0xBF5AF2, 0x30D158, 0xFF9F0A, 0xFF375F,
};
static const char *const accent_names[MOB_ACCENTS] = {
    "Blue", "Purple", "Green", "Orange", "Pink",
};
const char *const mob_wallpaper_names[MOB_WALLPAPERS] = {
    "Aurora", "Ocean", "Sunset", "Graphite",
};

uint32_t mob_accent_n(int i)
{
    return accents[i >= 0 && i < MOB_ACCENTS ? i : 0];
}

uint32_t mob_accent(void)
{
    return mob_accent_n(mob_prefs.accent);
}

void mob_theme(struct mob_theme *t)
{
    mob_theme_for(t, mob_prefs.dark);
}

void mob_theme_for(struct mob_theme *t, int dark)
{
    if (dark) {
        t->bg = 0x101218;
        t->fg = 0xFFFFFF;
        t->muted = 0xAEB6CC;
        t->card = 0x1C1C24;
        t->line = 0x2C2C34;
        t->key = 0x3A3A44;
    } else {
        t->bg = 0xF2F2F7;
        t->fg = 0x1C1C1E;
        t->muted = 0x6E6E73;
        t->card = 0xFFFFFF;
        t->line = 0xD8D8DE;
        t->key = 0xFFFFFF;
    }
}

char *mob_put(char *p, const char *s)
{
    while (*s)
        *p++ = *s++;
    *p = 0;
    return p;
}

char *mob_put_num(char *p, uint32_t v)
{
    char tmp[12];
    int n = 0;
    do {
        tmp[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (n)
        *p++ = tmp[--n];
    *p = 0;
    return p;
}

/* (the kernel's string.h has neither) */
static const char *find_char(const char *s, char c)
{
    for (; *s; s++)
        if (*s == c)
            return s;
    return 0;
}

static void shift_right(char *s)
{
    size_t n = strlen(s);
    for (size_t i = n + 1; i > 0; i--)
        s[i] = s[i - 1];
}

static void shift_left(char *s)
{
    for (; *s; s++)
        s[0] = s[1];
}

static int inside(int x, int y, int rx, int ry, int rw, int rh)
{
    return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

/* A line in sixteenths of a logical pixel's thickness */
static void stroke(int x0, int y0, int x1, int y1, int t, uint32_t c)
{
    gfx_line16(x0 * 16, y0 * 16, x1 * 16, y1 * 16, t, c, 255);
}

/* The keys whose signs the kernel's font has none of: shift (an arrow
   up), backspace (a key's outline pointing left), return (an arrow
   back and down) - drawn about their middle */
static void key_icon(int kind, int cx, int cy, uint32_t c)
{
    switch (kind) {
    case 1:                     /* shift */
        stroke(cx - 8, cy + 1, cx, cy - 8, 28, c);
        stroke(cx, cy - 8, cx + 8, cy + 1, 28, c);
        stroke(cx - 4, cy + 1, cx - 4, cy + 8, 28, c);
        stroke(cx + 4, cy + 1, cx + 4, cy + 8, 28, c);
        stroke(cx - 4, cy + 8, cx + 4, cy + 8, 28, c);
        break;
    case 2:                     /* backspace */
        stroke(cx - 11, cy, cx - 5, cy - 7, 28, c);
        stroke(cx - 11, cy, cx - 5, cy + 7, 28, c);
        stroke(cx - 5, cy - 7, cx + 10, cy - 7, 28, c);
        stroke(cx - 5, cy + 7, cx + 10, cy + 7, 28, c);
        stroke(cx + 10, cy - 7, cx + 10, cy + 7, 28, c);
        stroke(cx - 1, cy - 3, cx + 5, cy + 3, 24, c);
        stroke(cx - 1, cy + 3, cx + 5, cy - 3, 24, c);
        break;
    case 3:                     /* return */
        stroke(cx + 9, cy - 7, cx + 9, cy + 3, 28, c);
        stroke(cx + 9, cy + 3, cx - 9, cy + 3, 28, c);
        stroke(cx - 9, cy + 3, cx - 4, cy - 2, 28, c);
        stroke(cx - 9, cy + 3, cx - 4, cy + 8, 28, c);
        break;
    }
}

/* --- Notes ----------------------------------------------------------- */

#define NOTES_TEXT_X   22
#define NOTES_TEXT_Y   116      /* the first line's baseline */
#define NOTES_LINE     26
#define NOTES_PX       18
#define NOTES_KB_UP    236      /* the keyboard's top, from the bottom */
#define NOTES_KB_Y     (MOB_LH - NOTES_KB_UP)
#define NOTES_KEY_H    44
#define NOTES_KEY_STEP 50       /* from a row's top to the next */
#define NOTES_KEY_GAP  3        /* between keys; ten keys span the width */
#define NOTES_KEY_PITCH ((MOB_LW - 10) / 10)
#define NOTES_KEY_W    (NOTES_KEY_PITCH - NOTES_KEY_GAP)
#define NOTES_KEY_PX   18
#define NOTES_MAX      2000

enum { KB_LETTERS, KB_DIGITS };
enum { K_CHAR, K_SHIFT, K_BACK, K_LAYER, K_POLISH, K_SPACE, K_ENTER };

static char note[NOTES_MAX + 1];
static int note_len;
static int kb_layer, kb_shift, kb_polish;

static const char *const letters[3][10] = {
    { "q", "w", "e", "r", "t", "y", "u", "i", "o", "p" },
    { "a", "s", "d", "f", "g", "h", "j", "k", "l", 0 },
    { "z", "x", "c", "v", "b", "n", "m", 0, 0, 0 },
};
static const char *const polish[10] = {
    "ą", "ć", "ę", "ł", "ń", "ó", "ś", "ź", "ż", 0
};
static const char *const polish_upper[10] = {
    "Ą", "Ć", "Ę", "Ł", "Ń", "Ó", "Ś", "Ź", "Ż", 0
};
static const char *const digits[3][10] = {
    { "1", "2", "3", "4", "5", "6", "7", "8", "9", "0" },
    { "-", "/", ":", ";", "(", ")", "&", "@", "\"", "'" },
    { ".", ",", "?", "!", "#", "%", "+", 0, 0, 0 },
};

struct key {
    int x, y, w, kind;
    const char *text;           /* what it types, or its label */
};

/* The keyboard's keys, in order: rows of letters (or digits), the third
   between shift and backspace, then the bottom row */
static int keys(struct key *out)
{
    int n = 0;
    for (int r = 0; r < 3; r++) {
        const char *const *row = kb_layer == KB_DIGITS ? digits[r] :
                                 (r == 0 && kb_polish) ?
                                 (kb_shift ? polish_upper : polish) :
                                 letters[r];
        int count = 0;
        while (count < 10 && row[count])
            count++;
        int y = NOTES_KB_Y + r * NOTES_KEY_STEP;
        int x = (MOB_LW - count * NOTES_KEY_PITCH + 3) / 2;
        if (r == 2) {
            out[n++] = (struct key){ 4, y, 46, K_SHIFT, "" };
            out[n++] = (struct key){ MOB_LW - 50, y, 46, K_BACK, "" };
        }
        for (int i = 0; i < count; i++)
            out[n++] = (struct key){ x + i * NOTES_KEY_PITCH, y,
                                     NOTES_KEY_W, K_CHAR, row[i] };
    }
    int y = NOTES_KB_Y + 3 * NOTES_KEY_STEP;
    out[n++] = (struct key){ 4, y, 54, K_LAYER,
                             kb_layer == KB_DIGITS ? "ABC" : "123" };
    out[n++] = (struct key){ 62, y, 42, K_POLISH, "ą" };
    out[n++] = (struct key){ 108, y, MOB_LW - 210, K_SPACE, "space" };
    out[n++] = (struct key){ MOB_LW - 98, y, 32, K_CHAR, "." };
    out[n++] = (struct key){ MOB_LW - 62, y, 58, K_ENTER, "" };
    return n;
}

/* What Notes is typed: text, or "\b" to take the last character */
static void back(void);
static void note_type(const char *s)
{
    if (s[0] == '\b') {
        back();
        return;
    }
    int n = (int)strlen(s);
    if (note_len + n > NOTES_MAX)
        return;
    memcpy(note + note_len, s, (size_t)n);
    note_len += n;
    note[note_len] = 0;
}

static void back(void)
{
    /* A whole character: UTF-8's continuation bytes with it */
    while (note_len > 0) {
        unsigned char c = (unsigned char)note[--note_len];
        if ((c & 0xC0) != 0x80)
            break;
    }
    note[note_len] = 0;
}

/* The lines the text falls into at the page's width: where each starts
   and how long it is, a line breaking at its last space */
#define NOTES_LINES 64
static int lines_of(int *start, int *len)
{
    int n = 0, at = 0, max = MOB_LW - 2 * NOTES_TEXT_X;
    static char piece[NOTES_MAX + 1];
    while (at <= note_len && n < NOTES_LINES) {
        int end = at, space = -1;
        for (;;) {
            if (end == note_len || note[end] == '\n')
                break;
            int next = end + 1;
            while (next < note_len && ((unsigned char)note[next] & 0xC0) ==
                   0x80)
                next++;
            memcpy(piece, note + at, (size_t)(next - at));
            piece[next - at] = 0;
            if (end > at && ui_font_width(UI_SANS, NOTES_PX, piece) > max)
                break;
            if (note[end] == ' ')
                space = end;
            end = next;
        }
        int stop = end;
        if (end < note_len && note[end] != '\n' && space > at)
            stop = space;               /* the word goes to the next line */
        start[n] = at;
        len[n] = stop - at;
        n++;
        if (stop >= note_len)
            break;
        at = stop + 1;          /* past the space or the newline */
        if (stop == end && note[end] != '\n' && note[end] != ' ')
            at = stop;          /* a word longer than the line: cut */
    }
    return n;
}

void notes_draw(void)
{
    struct mob_theme t;
    mob_theme(&t);
    int lw = video_w(), lh = video_h();
    gfx_fill(0, 0, lw, lh, t.bg, 255);
    mob_status_bar(t.fg);
    mob_title("Notes", t.fg);

    int start[NOTES_LINES], len[NOTES_LINES];
    int n = lines_of(start, len);
    int fit = (NOTES_KB_Y - NOTES_TEXT_Y) / NOTES_LINE;
    int first = n > fit ? n - fit : 0;      /* the end stays in view */
    static char line[NOTES_MAX + 1];
    int cx = NOTES_TEXT_X, cy = NOTES_TEXT_Y;
    for (int i = first; i < n; i++) {
        memcpy(line, note + start[i], (size_t)len[i]);
        line[len[i]] = 0;
        int y = NOTES_TEXT_Y + (i - first) * NOTES_LINE;
        int end = ui_font_text(UI_SANS, NOTES_PX, NOTES_TEXT_X, y, line,
                               t.fg);
        cx = end;
        cy = y;
    }
    if (!note_len)
        ui_font_text(UI_SANS, NOTES_PX, NOTES_TEXT_X, NOTES_TEXT_Y,
                     "Type a note", t.muted);
    gfx_fill(cx + 1, cy - 16, 2, 20, mob_accent(), 255);  /* the cursor */
    mob_kb_draw();
    mob_pill(t.fg);
}

/* --- The keyboard: Notes', and programs' (mobile.c) -------------------- */

int mob_kb_top(void)
{
    return NOTES_KB_Y - 8;
}

void mob_kb_draw(void)
{
    struct mob_theme t;
    mob_theme(&t);
    int lw = video_w(), lh = video_h();
    gfx_fill(0, NOTES_KB_Y - 8, lw, lh - NOTES_KB_Y + 8, t.line, 255);
    struct key k[48];
    int count = keys(k);
    for (int i = 0; i < count; i++) {
        int on = (k[i].kind == K_SHIFT && kb_shift) ||
                 (k[i].kind == K_POLISH && kb_polish);
        uint32_t c = on ? mob_accent() : t.key;
        gfx_rrect(k[i].x, k[i].y, k[i].w, NOTES_KEY_H, 6, c, 255);
        int px = k[i].kind == K_CHAR ? NOTES_KEY_PX : 14;
        uint32_t ink = on ? 0xFFFFFF : t.fg;
        int kx = k[i].x + k[i].w / 2, ky = k[i].y + NOTES_KEY_H / 2;
        if (k[i].kind == K_SHIFT) {
            if (kb_layer == KB_LETTERS)     /* no capitals among digits */
                key_icon(1, kx, ky, ink);
            continue;
        }
        if (k[i].kind == K_BACK || k[i].kind == K_ENTER) {
            key_icon(k[i].kind == K_BACK ? 2 : 3, kx, ky, ink);
            continue;
        }
        mob_centred(UI_SANS, px, kx, ky + px / 3, k[i].text, ink);
    }
}

int mob_kb_tap(int x, int y, void (*emit)(const char *s))
{
    struct key k[48];
    int count = keys(k);
    for (int i = 0; i < count; i++) {
        if (!inside(x, y, k[i].x, k[i].y, k[i].w, NOTES_KEY_H))
            continue;
        switch (k[i].kind) {
        case K_CHAR: {
            /* a capital: ASCII by arithmetic, the Polish ones are keys
               of their own; shift once, as a phone's is */
            const char *s = k[i].text;
            char up[2] = { 0, 0 };
            if (kb_shift && kb_layer == KB_LETTERS && s[0] >= 'a' &&
                s[0] <= 'z' && !s[1]) {
                up[0] = (char)(s[0] - 'a' + 'A');
                s = up;
            }
            emit(s);
            kb_shift = 0;
            break;
        }
        case K_SPACE:  emit(" "); break;
        case K_ENTER:  emit("\n"); break;
        case K_BACK:   emit("\b"); break;
        case K_SHIFT:
            if (kb_layer == KB_LETTERS)
                kb_shift = !kb_shift;
            break;
        case K_LAYER:
            kb_layer = kb_layer == KB_LETTERS ? KB_DIGITS : KB_LETTERS;
            kb_shift = 0;
            break;
        case K_POLISH:
            kb_polish = !kb_polish;
            kb_layer = KB_LETTERS;
            break;
        }
        return 1;
    }
    return 0;
}

int notes_tap(int x, int y)
{
    return mob_kb_tap(x, y, note_type);
}

/* --- Calculator -------------------------------------------------------- */

#define CALC_EXPR_UP   94       /* the sum so far: its baseline, above
                                   the keys */
#define CALC_EXPR_PX   20
#define CALC_RESULT_UP 26
#define CALC_RESULT_PX 56
#define CALC_KEY_X     22
#define CALC_KEY_GAP   12       /* four keys span the width */
#define CALC_STEP_X    ((MOB_LW - 24) / 4)
#define CALC_KEY_W     (CALC_STEP_X - CALC_KEY_GAP)
#define CALC_KEYS_UP   378      /* the keys' top, from the bottom */
#define CALC_KEY_Y     (MOB_LH - CALC_KEYS_UP)
#define CALC_EXPR_Y    (CALC_KEY_Y - CALC_EXPR_UP)
#define CALC_RESULT_Y  (CALC_KEY_Y - CALC_RESULT_UP)
#define CALC_KEY_H     62
#define CALC_STEP_Y    70
#define CALC_KEY_PX    26

#define SCALE   1000000LL                    /* six places */
#define LIMIT   (1000000000000LL * SCALE)    /* twelve digits before */

static const char *const calc_keys[5][4] = {
    { "C", "±", "%", "÷" },
    { "7", "8", "9", "×" },
    { "4", "5", "6", "−" },
    { "1", "2", "3", "+" },
    { "0", "0", ".", "=" },
};

static char entry[24];          /* what is being typed */
static int entering, evaluated, failed;
static int64_t acc;
static const char *op;          /* the operation waiting, or 0 */
static char expr[64];           /* the line above the result */

/* A value as a phone shows it: no trailing zeros, no point without
   digits after it */
static void fmt(int64_t v, char *out)
{
    char *p = out;
    if (v < 0) {
        *p++ = '-';
        v = -v;
    }
    uint64_t whole = (uint64_t)v / SCALE, part = (uint64_t)v % SCALE;
    char tmp[24];
    int n = 0;
    do {
        tmp[n++] = (char)('0' + whole % 10);
        whole /= 10;
    } while (whole);
    while (n)
        *p++ = tmp[--n];
    if (part) {
        *p++ = '.';
        for (uint64_t d = SCALE / 10; d && part; d /= 10) {
            *p++ = (char)('0' + part / d);
            part %= d;
        }
    }
    *p = 0;
}

static int64_t parse(const char *s)
{
    int neg = *s == '-';
    if (neg)
        s++;
    int64_t whole = 0, part = 0, d = SCALE / 10;
    for (; *s && *s != '.'; s++)
        whole = whole * 10 + (*s - '0');
    if (*s == '.')
        for (s++; *s && d; s++, d /= 10)
            part += (*s - '0') * d;
    int64_t v = whole * SCALE + part;
    return neg ? -v : v;
}

static int mul(int64_t a, int64_t b, int64_t *out)
{
    int neg = (a < 0) != (b < 0);
    uint64_t x = (uint64_t)(a < 0 ? -a : a), y = (uint64_t)(b < 0 ? -b : b);
    uint64_t xi = x / SCALE, xf = x % SCALE, yi = y / SCALE, yf = y % SCALE;
    uint64_t r, t;
    /* x*y/S = xi*yi*S + xi*yf + xf*yi + xf*yf/S, rounded half up */
    if (__builtin_mul_overflow(xi, yi, &r) ||
        __builtin_mul_overflow(r, (uint64_t)SCALE, &r))
        return -1;
    t = xi * yf + xf * yi + (xf * yf + SCALE / 2) / SCALE;
    if (__builtin_add_overflow(r, t, &r) || r >= (uint64_t)LIMIT)
        return -1;
    *out = neg ? -(int64_t)r : (int64_t)r;
    return 0;
}

static int divide(int64_t a, int64_t b, int64_t *out)
{
    if (!b)
        return -1;
    int neg = (a < 0) != (b < 0);
    uint64_t x = (uint64_t)(a < 0 ? -a : a), y = (uint64_t)(b < 0 ? -b : b);
    uint64_t q = x / y, rem = x % y;
    if (q >= (uint64_t)(LIMIT / SCALE))
        return -1;
    uint64_t r = q * SCALE;
    for (uint64_t d = SCALE / 10; d; d /= 10) {
        rem *= 10;                     /* below 10^19: in 64 bits */
        r += rem / y * d;
        rem %= y;
    }
    if (rem * 10 / y >= 5)
        r++;
    if (r >= (uint64_t)LIMIT)
        return -1;
    *out = neg ? -(int64_t)r : (int64_t)r;
    return 0;
}

static int apply(int64_t a, const char *o, int64_t b, int64_t *out)
{
    if (!strcmp(o, "+") || !strcmp(o, "−")) {
        int64_t r = !strcmp(o, "+") ? a + b : a - b;
        if (r >= LIMIT || r <= -LIMIT)
            return -1;
        *out = r;
        return 0;
    }
    return !strcmp(o, "×") ? mul(a, b, out) : divide(a, b, out);
}

static void calc_clear(void)
{
    entry[0] = 0;
    entering = evaluated = failed = 0;
    acc = 0;
    op = 0;
    expr[0] = 0;
}

static int64_t shown(void)
{
    return entering ? parse(entry) : acc;
}

static void calc_key(const char *k)
{
    char a[32], b[32], r[32];
    if (failed && strcmp(k, "C"))
        calc_clear();
    if (k[0] >= '0' && k[0] <= '9' && !k[1]) {
        if (evaluated)
            calc_clear();
        if (!entering) {
            entry[0] = 0;
            entering = 1;
        }
        int digits_in = 0, after = -1;
        for (const char *p = entry; *p; p++) {
            if (*p >= '0' && *p <= '9') {
                digits_in++;
                if (after >= 0)
                    after++;
            }
            if (*p == '.')
                after = 0;
        }
        if (digits_in >= 12 || after >= 6)
            return;
        if (!strcmp(entry, "0") || !strcmp(entry, "-0"))
            entry[strlen(entry) - 1] = 0;
        size_t n = strlen(entry);
        entry[n] = k[0];
        entry[n + 1] = 0;
        return;
    }
    if (!strcmp(k, ".")) {
        if (evaluated)
            calc_clear();
        if (!entering) {
            mob_put(entry, "0");
            entering = 1;
        }
        if (!find_char(entry, '.'))
            mob_put(entry + strlen(entry), ".");
        return;
    }
    if (!strcmp(k, "C")) {
        calc_clear();
        return;
    }
    if (!strcmp(k, "±")) {
        if (entering) {
            if (entry[0] == '-') {
                shift_left(entry);
            } else {
                shift_right(entry);
                entry[0] = '-';
            }
        } else {
            acc = -acc;
        }
        return;
    }
    if (!strcmp(k, "%")) {
        int64_t v = shown() / 100;
        if (entering)
            fmt(v, entry);
        else
            acc = v;
        return;
    }
    if (!strcmp(k, "=")) {
        if (!op)
            return;
        int64_t v = shown(), res = 0;
        fmt(acc, a);
        fmt(v, b);
        failed = apply(acc, op, v, &res) < 0;
        if (failed)
            mob_put(r, "Error");
        else
            fmt(res, r);
        printk("[calc] %s %s %s = %s\n", a, op, b, r);
        char *p = mob_put(expr, a);
        p = mob_put(p, " ");
        p = mob_put(p, op);
        p = mob_put(p, " ");
        p = mob_put(p, b);
        mob_put(p, " =");
        acc = res;
        op = 0;
        entering = 0;
        evaluated = 1;
        return;
    }
    /* + − × ÷: what is there so far, then the operation */
    if (op && entering) {
        int64_t res;
        if (apply(acc, op, parse(entry), &res) < 0) {
            failed = 1;
            op = 0;
            entering = 0;
            return;
        }
        acc = res;
    } else if (entering) {
        acc = parse(entry);
    }
    op = k;
    entering = 0;
    evaluated = 0;
    fmt(acc, a);
    char *p = mob_put(expr, a);
    p = mob_put(p, " ");
    mob_put(p, op);
}

static void calc_rect(int r, int c, int *x, int *y, int *w)
{
    *x = CALC_KEY_X + c * CALC_STEP_X;
    *y = CALC_KEY_Y + r * CALC_STEP_Y;
    *w = CALC_KEY_W;
    if (r == 4 && c == 0)
        *w = CALC_STEP_X + CALC_KEY_W;      /* 0 takes two places */
}

void calc_draw(void)
{
    struct mob_theme t;
    mob_theme(&t);
    int lw = video_w(), lh = video_h();
    gfx_fill(0, 0, lw, lh, t.bg, 255);
    mob_status_bar(t.fg);
    mob_title("Calculator", t.fg);
    ui_font_text(UI_SANS, CALC_EXPR_PX, lw - 22 - ui_font_width(UI_SANS,
                 CALC_EXPR_PX, expr), CALC_EXPR_Y, expr, t.muted);
    char s[32];
    if (failed)
        mob_put(s, "Error");
    else if (entering)
        mob_put(s, entry);
    else
        fmt(acc, s);
    int px = CALC_RESULT_PX;
    while (px > 24 && ui_font_width(UI_SANS, px, s) > lw - 44)
        px -= 4;
    ui_font_text(UI_SANS, px, lw - 22 - ui_font_width(UI_SANS, px, s),
                 CALC_RESULT_Y, s, t.fg);
    for (int r = 0; r < 5; r++)
        for (int c = 0; c < 4; c++) {
            if (r == 4 && c == 1)
                continue;                   /* the 0's second place */
            int x, y, w;
            calc_rect(r, c, &x, &y, &w);
            const char *k = calc_keys[r][c];
            uint32_t bg = c == 3 ? ORANGE : r == 0 ? 0xA5A5A5 :
                          mob_prefs.dark ? 0x333333 : 0xE5E5EA;
            uint32_t fg = c == 3 ? 0xFFFFFF : r == 0 ? 0x1C1C1E : t.fg;
            if (c == 3 && op && !entering && !strcmp(op, k)) {
                bg = 0xFFFFFF;              /* the operation waiting */
                fg = ORANGE;
            }
            gfx_rrect(x, y, w, CALC_KEY_H, CALC_KEY_H / 2, bg, 255);
            if (!strcmp(k, "−"))            /* a sign the font lacks */
                gfx_fill(x + w / 2 - 9, y + CALC_KEY_H / 2 - 1, 18, 3, fg,
                         255);
            else
                mob_centred(UI_SANS, CALC_KEY_PX, x + w / 2,
                            y + CALC_KEY_H / 2 + CALC_KEY_PX / 3, k, fg);
        }
    mob_pill(t.fg);
}

int calc_tap(int x, int y)
{
    for (int r = 0; r < 5; r++)
        for (int c = 0; c < 4; c++) {
            if (r == 4 && c == 1)
                continue;
            int kx, ky, w;
            calc_rect(r, c, &kx, &ky, &w);
            if (inside(x, y, kx, ky, w, CALC_KEY_H)) {
                calc_key(calc_keys[r][c]);
                return 1;
            }
        }
    return 0;
}

/* --- Settings ------------------------------------------------------------ */

#define SET_CARD_Y     96       /* the rows' card */
#define SET_ROW_H      52
#define SET_GLASS_R    24       /* the card's corners, in Liquid Glass */
#define SET_PX         16
#define SET_SWITCH_W   50
#define SET_SWITCH_H   30
#define SET_SLIDER_X   170
#define SET_SLIDER_W   150
#define SET_BACK_W     120      /* a page's way back, beside its title */
#define SET_DOT_X      150      /* the accents' first colour */
#define SET_DOT_STEP   38

enum { P_MAIN, P_NETWORK, P_DISPLAY, P_SOUND, P_BATTERY, P_NAV, P_TIME,
       P_SIZE, P_WALLPAPER, P_LOOK, PAGES };
enum { R_LINK, R_SWITCH, R_SLIDER, R_CHOICE, R_VALUE, R_ACCENTS, R_ABOUT,
       R_NOTE };
enum { V_ADDRESS, V_STATE, V_BATTERY, V_POWER, V_SPEAKER, V_ZONE, V_CARD };

struct srow {
    const char *label;
    int kind;
    int *pref;                  /* a switch's, a slider's, a choice's */
    int arg;                    /* a link's page, a choice's value, a
                                   value's which */
};

#define P(f) (&mob_prefs.f)

static const struct srow main_rows[] = {
    { "Airplane mode", R_SWITCH, P(airplane), 0 },
    { "Network", R_LINK, 0, P_NETWORK },
    { "Display", R_LINK, 0, P_DISPLAY },
    { "Appearance", R_LINK, 0, P_LOOK },
    { "Sound & notifications", R_LINK, 0, P_SOUND },
    { "Battery", R_LINK, 0, P_BATTERY },
    { "Navigation", R_LINK, 0, P_NAV },
    { "Language & time", R_LINK, 0, P_TIME },
    { "About phone", R_ABOUT, 0, 0 },
};
static const struct srow network_rows[] = {
    { "Status", R_VALUE, 0, V_STATE },
    { "Address", R_VALUE, 0, V_ADDRESS },
    { "Card", R_VALUE, 0, V_CARD },
    { "Airplane mode", R_SWITCH, P(airplane), 0 },
};
static const struct srow display_rows[] = {
    { "Brightness", R_SLIDER, P(brightness), 0 },
    { "Display size", R_LINK, 0, P_SIZE },
    { "Portrait", R_CHOICE, P(landscape), 0 },
    { "Landscape", R_CHOICE, P(landscape), 1 },
};
static const struct srow look_rows[] = {
    { "Matte", R_CHOICE, P(theme), MOB_THEME_MATTE },
    { "Liquid Glass", R_CHOICE, P(theme), MOB_THEME_GLASS },
    { "Solid", R_CHOICE, P(theme), MOB_THEME_SOLID },
    { "Dark mode", R_SWITCH, P(dark), 0 },
    { "Wallpaper", R_LINK, 0, P_WALLPAPER },
    { "Accent", R_ACCENTS, P(accent), 0 },
    { "Round icons", R_SWITCH, P(round_icons), 0 },
    { "Themes change the dock, the shade, banners, Settings.", R_NOTE,
      0, 0 },
};
static const struct srow size_rows[] = {
    { "Large, 200%", R_CHOICE, P(size), 0 },
    { "Medium, 175%", R_CHOICE, P(size), 1 },
    { "Small, 150%", R_CHOICE, P(size), 2 },
    { "Smaller shows more on the screen.", R_NOTE, 0, 0 },
};
static const struct srow wallpaper_rows[] = {
    { "Aurora", R_CHOICE, P(wallpaper), 0 },
    { "Ocean", R_CHOICE, P(wallpaper), 1 },
    { "Sunset", R_CHOICE, P(wallpaper), 2 },
    { "Graphite", R_CHOICE, P(wallpaper), 3 },
};
static const struct srow sound_rows[] = {
    { "Speaker", R_VALUE, 0, V_SPEAKER },
    { "Silent mode", R_SWITCH, P(silent), 0 },
    { "Volume", R_SLIDER, P(volume), 0 },
    { "Notification banners", R_SWITCH, P(banners), 0 },
    { "On the lock screen", R_SWITCH, P(lock_notes), 0 },
    { "Do not disturb", R_SWITCH, P(dnd), 0 },
};
static const struct srow battery_rows[] = {
    { "Level", R_VALUE, 0, V_BATTERY },
    { "Power", R_VALUE, 0, V_POWER },
    { "Battery saver", R_SWITCH, P(saver), 0 },
    { "Percentage in the bar", R_SWITCH, P(show_pct), 0 },
};
static const struct srow nav_rows[] = {
    { "Gestures, as an iPhone", R_CHOICE, P(nav_buttons), 0 },
    { "Three buttons, as Android", R_CHOICE, P(nav_buttons), 1 },
    { "Gestures: up from the bottom edge for home.", R_NOTE, 0, 0 },
    { "Buttons: back, home and recent apps.", R_NOTE, 0, 0 },
};
static const struct srow time_rows[] = {
    { "24-hour clock", R_SWITCH, P(clock24), 0 },
    { "Time zone", R_VALUE, 0, V_ZONE },
    { "Language: English", R_CHOICE, P(language), 0 },
    { "Language: Polish", R_CHOICE, P(language), 1 },
    { "Programs and web pages are told it.", R_NOTE, 0, 0 },
};

#define ROWS(r) r, (int)(sizeof(r) / sizeof(r[0]))

static const struct spage {
    const char *title;
    const struct srow *rows;
    int count;
    int parent;                 /* where back goes */
} pages[PAGES] = {
    { "Settings", ROWS(main_rows), P_MAIN },
    { "Network", ROWS(network_rows), P_MAIN },
    { "Display", ROWS(display_rows), P_MAIN },
    { "Sound", ROWS(sound_rows), P_MAIN },
    { "Battery", ROWS(battery_rows), P_MAIN },
    { "Navigation", ROWS(nav_rows), P_MAIN },
    { "Language & time", ROWS(time_rows), P_MAIN },
    { "Display size", ROWS(size_rows), P_DISPLAY },
    { "Wallpaper", ROWS(wallpaper_rows), P_LOOK },
    { "Appearance", ROWS(look_rows), P_MAIN },
};

static int page;

void settings_reset(void)
{
    page = P_MAIN;
}

int settings_back(void)
{
    if (page == P_MAIN)
        return 0;
    page = pages[page].parent;
    printk("[settings] %s\n", pages[page].title);
    return 1;
}

static void switch_drawn(int x, int y, int on, const struct mob_theme *t)
{
    gfx_rrect(x, y, SET_SWITCH_W, SET_SWITCH_H, SET_SWITCH_H / 2,
              on ? mob_accent() : t->line, 255);
    int kx = on ? x + SET_SWITCH_W - SET_SWITCH_H / 2 : x + SET_SWITCH_H / 2;
    gfx_disc16(kx * 16, (y + SET_SWITCH_H / 2) * 16, 13 * 16, 0xFFFFFF,
               255);
}

static void value_right(int baseline, const char *s, uint32_t c)
{
    ui_font_text(UI_SANS, SET_PX, MOB_LW - 32 - ui_font_width(UI_SANS,
                 SET_PX, s), baseline, s, c);
}

static void chevron(int cx, int cy, int left, uint32_t c)
{
    int d = left ? -3 : 3;
    stroke(cx - d, cy - 6, cx + d, cy, 28, c);
    stroke(cx + d, cy, cx - d, cy + 6, 28, c);
}

static void value_text(int which, char *s)
{
    char addr[20], *p;
    int charging, pct, st;
    switch (which) {
    case V_ADDRESS:
        st = mob_network(addr, sizeof(addr));
        mob_put(s, st == 2 ? addr : "None");
        break;
    case V_STATE:
        st = mob_network(addr, sizeof(addr));
        mob_put(s, st == 2 ? "Connected" : st == 3 ? "Off" :
                   st == 1 ? "Connecting" : "No network");
        break;
    case V_CARD:
        mob_put(s, mob_network(addr, sizeof(addr)) ? "virtio-net" : "None");
        break;
    case V_BATTERY:
        pct = mob_battery(&charging);
        p = mob_put_num(s, (uint32_t)(pct < 0 ? 100 : pct));
        mob_put(p, "%");
        break;
    case V_POWER:
        pct = mob_battery(&charging);
        mob_put(s, pct < 0 ? "Mains, no battery" :
                   charging ? "Charging" : "On battery");
        break;
    case V_SPEAKER:
        mob_put(s, vsnd_present() ? "virtio-sound" : "None found");
        break;
    case V_ZONE:
        mob_put(s, "UTC, as the clock");
        break;
    }
}

void settings_draw(void)
{
    struct mob_theme t;
    mob_theme(&t);
    const struct spage *pg = &pages[page];
    int lw = video_w(), lh = video_h();
    /* Under Liquid Glass the pages are cards of glass on the wallpaper,
       blurred and darkened - what is written on them light, whichever
       of the looks */
    int glass = mob_glass_backdrop();
    if (glass) {
        t.fg = 0xFFFFFF;
        t.muted = 0xAEB6CC;
        t.line = 0x6A6E7A;
    } else {
        gfx_fill(0, 0, lw, lh, t.bg, 255);
    }
    mob_status_bar(t.fg);
    if (page == P_MAIN) {
        mob_title(pg->title, t.fg);
    } else {
        chevron(26, APP_TITLE_Y - 8, 1, mob_accent());
        ui_font_text(UI_BOLD, 22, 40, APP_TITLE_Y, pg->title, t.fg);
    }
    int rows = 0, notes_seen = 0;
    for (int i = 0; i < pg->count; i++)
        rows += pg->rows[i].kind != R_NOTE;
    uint32_t below = t.muted;        /* under the card, on the page */
    if (glass) {
        /* what is written on the glass is dark where it came out light,
           as on Weft's toolbar */
        if (mob_glass_card(12, SET_CARD_Y, lw - 24, rows * SET_ROW_H,
                           SET_GLASS_R) > 150) {
            t.fg = 0x1C1C1E;
            t.muted = 0x48484E;
            t.line = 0x9A9AA2;
        }
    } else {
        gfx_rrect(12, SET_CARD_Y, lw - 24, rows * SET_ROW_H, 14, t.card,
                  255);
    }
    for (int i = 0, k = 0; i < pg->count; i++) {
        const struct srow *r = &pg->rows[i];
        if (r->kind == R_NOTE) {
            /* explanations below the card */
            int y = SET_CARD_Y + rows * SET_ROW_H + 26 + notes_seen++ * 22;
            ui_font_text(UI_SANS, 13, 22, y, r->label, below);
            continue;
        }
        int top = SET_CARD_Y + k * SET_ROW_H;
        int base = top + SET_ROW_H / 2 + 6, cy = top + SET_ROW_H / 2;
        if (k)
            gfx_fill(28, top, lw - 40, 1, t.line, 255);
        k++;
        ui_font_text(UI_SANS, SET_PX, 28, base, r->label, t.fg);
        char s[40];
        switch (r->kind) {
        case R_LINK:
        case R_ABOUT:
            chevron(MOB_LW - 38, cy, 0, t.muted);
            break;
        case R_SWITCH:
            switch_drawn(lw - 32 - SET_SWITCH_W, top + (SET_ROW_H -
                         SET_SWITCH_H) / 2, *r->pref, &t);
            break;
        case R_SLIDER: {
            int fill = SET_SLIDER_W * *r->pref / 100;
            gfx_rrect(SET_SLIDER_X, cy - 3, SET_SLIDER_W, 6, 3, t.line, 255);
            gfx_rrect(SET_SLIDER_X, cy - 3, fill, 6, 3, mob_accent(), 255);
            gfx_disc16((SET_SLIDER_X + fill) * 16, cy * 16, 12 * 16, t.line,
                       255);
            gfx_disc16((SET_SLIDER_X + fill) * 16, cy * 16, 11 * 16,
                       0xFFFFFF, 255);
            break;
        }
        case R_CHOICE:
            if (*r->pref == r->arg) {
                /* a tick */
                int x = MOB_LW - 44;
                stroke(x, cy, x + 5, cy + 6, 32, mob_accent());
                stroke(x + 5, cy + 6, x + 15, cy - 7, 32, mob_accent());
            }
            break;
        case R_VALUE:
            value_text(r->arg, s);
            value_right(base, s, t.muted);
            break;
        case R_ACCENTS:
            for (int a = 0; a < MOB_ACCENTS; a++) {
                int dx = SET_DOT_X + a * SET_DOT_STEP;
                if (a == *r->pref)
                    gfx_disc16(dx * 16, cy * 16, 15 * 16, t.fg, 255);
                gfx_disc16(dx * 16, cy * 16, 12 * 16, mob_accent_n(a), 255);
            }
            break;
        }
    }
    mob_pill(t.fg);
}

/* A setting changed: said on the serial port, and done where it is
   more than a value to keep */
static void changed(const struct srow *r, int value)
{
    if (r->pref == P(airplane)) {
        mob_set_airplane(value);
        return;
    }
    if (r->pref == P(brightness)) {
        mob_set_brightness(value);
        return;
    }
    *r->pref = value;
    if (r->pref == P(saver))
        mob_apply_saver();
    if (r->pref == P(wallpaper))
        mob_wallpaper_changed();
    if (r->pref == P(size))
        mob_set_size(value);
    if (r->pref == P(nav_buttons))
        mob_windows_fit();
    if (r->pref == P(landscape))
        mob_set_orientation(value);
    if (r->pref == P(volume))
        vsnd_volume(value, -1);
    if (r->kind == R_CHOICE && r->pref == P(theme))
        printk("[settings] theme %s\n", r->label);
    else if (r->kind == R_CHOICE && r->pref == P(wallpaper))
        printk("[settings] wallpaper %s\n", mob_wallpaper_names[value]);
    else if (r->kind == R_ACCENTS)
        printk("[settings] accent %s\n", accent_names[value]);
    else if (r->kind == R_CHOICE)
        printk("[settings] %s\n", r->label);
    else if (r->kind == R_SLIDER)
        printk("[settings] %s %d%%\n", r->label, value);
    else
        printk("[settings] %s %s\n", r->label, value ? "on" : "off");
}

int settings_tap(int x, int y)
{
    const struct spage *pg = &pages[page];
    if (page != P_MAIN && y >= APP_TITLE_Y - 30 && y < APP_TITLE_Y + 10 &&
        x < SET_BACK_W)
        return settings_back();
    if (y < SET_CARD_Y)
        return 0;
    int k = (y - SET_CARD_Y) / SET_ROW_H;
    for (int i = 0, n = 0; i < pg->count; i++) {
        const struct srow *r = &pg->rows[i];
        if (r->kind == R_NOTE)
            continue;
        if (n++ != k)
            continue;
        switch (r->kind) {
        case R_LINK:
            page = r->arg;
            printk("[settings] %s\n", pages[page].title);
            return 1;
        case R_ABOUT:
            return SETTINGS_ABOUT;
        case R_SWITCH:
            changed(r, !*r->pref);
            return 1;
        case R_SLIDER: {
            if (x < SET_SLIDER_X - 12)
                return 0;
            int v = (x - SET_SLIDER_X) * 100 / SET_SLIDER_W;
            changed(r, v < 0 ? 0 : v > 100 ? 100 : v);
            return 1;
        }
        case R_CHOICE:
            changed(r, r->arg);
            return 1;
        case R_ACCENTS:
            for (int a = 0; a < MOB_ACCENTS; a++) {
                int dx = SET_DOT_X + a * SET_DOT_STEP;
                if (x >= dx - 17 && x < dx + 17) {
                    changed(r, a);
                    return 1;
                }
            }
            return 0;
        }
        return 0;
    }
    return 0;
}
