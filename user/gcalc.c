/* gcalc - a calculator (ring 3)
 *
 * Four operations, a percent, a change of sign, and a display that
 * says what is being worked out above the number itself. Clicked or
 * typed: the digits, + - * /, Enter or =, Backspace, Esc to clear.
 *
 * The numbers are not floating point. They are whole numbers counted in
 * millionths, in 64 bits - so 0.1 + 0.2 is 0.3, which a calculator
 * that shows ten digits owes the person reading it, and which binary
 * floating point does not give. Six decimals and twelve digits before
 * the point is a desk calculator's range; past it the display says
 * "Error" rather than something wrong.
 *
 * A 32-bit program dividing 64-bit numbers would need the compiler's
 * library, which a program here does not have, so the division is done
 * here, a bit at a time. One source for both kernels (uapi.h).
 */

#include "udraw.h"
#include "ulocale.h"

#define W0 260
#define H0 364
#define PAD 14

#define DISP_H   92           /* the display, at the top */
#define DISP_PX  32           /* the number's size */
#define DISP_BASE 78          /* its baseline */
#define EXPR_BASE 30          /* the line above it: what is being worked out */

#define KEY_X0   14           /* the keys: five rows of four */
#define KEY_Y0   104
#define KEY_W    52
#define KEY_H    44
#define KEY_GAP  8

#define SCALE    1000000      /* millionths */
#define DIGITS   12           /* before the point, at most */
#define ENTRY_MAX 20

typedef long long num_t;

/* The keys, row by row; "0" takes two places */
static const char *const keys[5][4] = {
    { "C", "+/-", "%", "/" },
    { "7", "8", "9", "*" },
    { "4", "5", "6", "-" },
    { "1", "2", "3", "+" },
    { "0", "", ".", "=" },
};

static char entry[ENTRY_MAX + 1] = "0";   /* what is being typed */
static int entering;          /* digits are going into 'entry' */
static num_t acc;             /* the number so far */
static char op;               /* the operation waiting for its second number */
static int failed;            /* the display says "Error" */
static char expr[48];         /* the line above: "12 +" */

/* --- arithmetic in millionths ------------------------------------------ */

static unsigned long long udiv(unsigned long long a, unsigned long long b,
                               unsigned long long *rem)
{
    unsigned long long q = 0, r = 0;
    for (int i = 63; i >= 0; i--) {
        r = (r << 1) | ((a >> i) & 1);
        if (r >= b) {
            r -= b;
            q |= 1ULL << i;
        }
    }
    if (rem)
        *rem = r;
    return q;
}

static int too_big(num_t v)
{
    num_t lim = 999999999999LL * SCALE;
    return v > lim || v < -lim;
}

/* a * b / SCALE, rounded, or 0 with *bad set when it does not fit */
static num_t mul(num_t a, num_t b, int *bad)
{
    int neg = (a < 0) != (b < 0);
    unsigned long long x = (unsigned long long)(a < 0 ? -a : a);
    unsigned long long y = (unsigned long long)(b < 0 ? -b : b);
    /* x * y can pass 64 bits: split y into its whole millions and the
       rest, so that nothing multiplied is larger than it has to be */
    unsigned long long yr, yq = udiv(y, SCALE, &yr);
    if (yq && x > udiv(0xFFFFFFFFFFFFFFFFULL, yq, 0)) {
        *bad = 1;
        return 0;
    }
    unsigned long long part = x * yq;
    unsigned long long xr, xq = udiv(x, SCALE, &xr);
    unsigned long long low = xq * yr + udiv(xr * yr + SCALE / 2, SCALE, 0);
    unsigned long long r = part + low;
    if (r < part || r > 0x7FFFFFFFFFFFFFFFULL) {
        *bad = 1;
        return 0;
    }
    return neg ? -(num_t)r : (num_t)r;
}

/* a * SCALE / b, rounded */
static num_t divide(num_t a, num_t b, int *bad)
{
    if (b == 0) {
        *bad = 1;
        return 0;
    }
    int neg = (a < 0) != (b < 0);
    unsigned long long x = (unsigned long long)(a < 0 ? -a : a);
    unsigned long long y = (unsigned long long)(b < 0 ? -b : b);
    /* long division: the whole part, then six decimal places, each a
       remainder times ten */
    unsigned long long rem, q = udiv(x, y, &rem);
    for (int i = 0; i < 6; i++) {
        if (q > 0x7FFFFFFFFFFFFFFFULL / 16) {
            *bad = 1;
            return 0;
        }
        unsigned long long d = udiv(rem * 10, y, &rem);
        q = q * 10 + d;
    }
    if (rem * 2 >= y)
        q++;
    if (q > 0x7FFFFFFFFFFFFFFFULL) {
        *bad = 1;
        return 0;
    }
    return neg ? -(num_t)q : (num_t)q;
}

static num_t apply(num_t a, char o, num_t b, int *bad)
{
    switch (o) {
    case '+': return a + b;
    case '-': return a - b;
    case '*': return mul(a, b, bad);
    case '/': return divide(a, b, bad);
    }
    return b;
}

/* "12.5" -> 12500000 */
static num_t parse(const char *s)
{
    int neg = *s == '-';
    if (neg)
        s++;
    num_t v = 0, frac = 0;
    int places = 0, dot = 0;
    for (; *s; s++) {
        if (*s == '.' || *s == ',') {
            dot = 1;
        } else if (!dot) {
            v = v * 10 + (*s - '0');
        } else if (places < 6) {
            frac = frac * 10 + (*s - '0');
            places++;
        }
    }
    while (places++ < 6)
        frac *= 10;
    v = v * SCALE + frac;
    return neg ? -v : v;
}

/* 12500000 -> "12.5": no trailing zeros, no point when there is
   nothing after it */
static void format(num_t v, char *out)
{
    int k = 0;
    if (v < 0) {
        out[k++] = '-';
        v = -v;
    }
    unsigned long long rem, whole = udiv((unsigned long long)v, SCALE, &rem);
    char tmp[24];
    int n = 0;
    do {
        unsigned long long d;
        whole = udiv(whole, 10, &d);
        tmp[n++] = (char)('0' + d);
    } while (whole);
    while (n)
        out[k++] = tmp[--n];
    if (rem) {
        char f[7];
        for (int i = 5; i >= 0; i--) {
            unsigned long long d;
            rem = udiv(rem, 10, &d);
            f[i] = (char)('0' + d);
        }
        int last = 5;
        while (last > 0 && f[last] == '0')
            last--;
        out[k++] = ulocale_decimal_comma() ? ',' : '.';
        for (int i = 0; i <= last; i++)
            out[k++] = f[i];
    }
    out[k] = '\0';
}

static int slen(const char *s)
{
    int n = 0;
    while (s[n])
        n++;
    return n;
}

static void show(num_t v)
{
    format(v, entry);
}

static const char *op_sign(char o)
{
    switch (o) {
    case '+': return "+";
    case '-': return "-";                 /* (the font has no minus sign) */
    case '*': return "\xC3\x97";          /* the times sign */
    case '/': return "\xC3\xB7";          /* the division sign */
    }
    return "";
}

static void set_expr(num_t a, char o)
{
    format(a, expr);
    if (o) {
        int n = slen(expr);
        expr[n++] = ' ';
        const char *sg = op_sign(o);
        for (int i = 0; sg[i]; i++)
            expr[n++] = sg[i];
        expr[n] = '\0';
    }
}

static void clear(void)
{
    entry[0] = '0';
    entry[1] = '\0';
    entering = 0;
    acc = 0;
    op = 0;
    failed = 0;
    expr[0] = '\0';
}

static void fail(void)
{
    clear();
    failed = 1;
}

/* One key, clicked or typed */
static void press(const char *k)
{
    char c = k[0];
    if (failed && c != 'C') {
        clear();
        if (!(c >= '0' && c <= '9') && c != '.')
            return;
    }
    if (c >= '0' && c <= '9') {
        if (!entering) {
            entry[0] = '\0';
            entering = 1;
        }
        int n = slen(entry), digits = 0, dot = 0;
        for (int i = 0; i < n; i++) {
            if (entry[i] == '.')
                dot = 1;
            else if (entry[i] != '-' && !dot)
                digits++;
        }
        if (n >= ENTRY_MAX || (!dot && digits >= DIGITS))
            return;
        if (n == 1 && entry[0] == '0')
            n = 0;                       /* no leading zero */
        entry[n++] = c;
        entry[n] = '\0';
    } else if (c == '.') {
        if (!entering) {
            entry[0] = '0';
            entry[1] = '\0';
            entering = 1;
        }
        for (int i = 0; entry[i]; i++)
            if (entry[i] == '.')
                return;
        int n = slen(entry);
        entry[n++] = '.';
        entry[n] = '\0';
    } else if (c == 'C') {
        clear();
    } else if (k[0] == '+' && k[1] == '/') {      /* +/- */
        num_t v = -parse(entry);
        show(v);
        if (!entering)
            acc = v;
    } else if (c == '%') {
        int bad = 0;
        num_t v = divide(parse(entry), 100LL * SCALE, &bad);
        if (bad) {
            fail();
            return;
        }
        show(v);
        entering = 0;
    } else if (c == '<') {                         /* Backspace */
        if (!entering)
            return;
        int n = slen(entry);
        if (n > 1)
            entry[n - 1] = '\0';
        else
            entry[0] = '0';
    } else if (c == '+' || c == '-' || c == '*' || c == '/' || c == '=') {
        num_t v = parse(entry);
        int bad = 0;
        if (op && (entering || c == '=')) {
            acc = apply(acc, op, v, &bad);
        } else if (!op) {
            acc = v;
        }
        if (bad || too_big(acc)) {
            fail();
            return;
        }
        if (c == '=') {
            if (op) {
                set_expr(acc, 0);
                expr[0] = '\0';
            }
            op = 0;
        } else {
            op = c;
            set_expr(acc, op);
        }
        show(acc);
        entering = 0;
    }
}

/* --- drawing ------------------------------------------------------------- */

static int key_x(int c) { return KEY_X0 + c * (KEY_W + KEY_GAP); }
static int key_y(int r) { return KEY_Y0 + r * (KEY_H + KEY_GAP); }

static void draw(int win, int cw, int ch)
{
    ulocale_window_title(win, "Calculator", "Kalkulator");
    struct ud_look l;
    ud_look(&l);
    ud_fill(win, 0, 0, cw, ch, l.bg);

    /* The display: what is being worked out, and the number */
    ud_round(win, 8, 8, cw - 16, DISP_H - 8, 12, l.field);
    if (expr[0])
        ud_text_right(win, cw - PAD - 4, EXPR_BASE, expr, l.dim, UD_SANS, 14);
    char shown[ENTRY_MAX + 1];
    const char *big = entry;
    if (failed) {
        ud_text_mid_l(win, 10, 8, W0 - 20, DISP_H, "Error", l.danger,
                      UD_BOLD, DISP_PX);
        big = (const char *)0;
    }
    if (!failed && ulocale_decimal_comma()) {
        int i = 0;
        for (; entry[i]; i++)
            shown[i] = entry[i] == '.' ? ',' : entry[i];
        shown[i] = 0;
        big = shown;
    }
    if (big) {
        int px = DISP_PX;
        while (px > 14 && ud_width(big, UD_SANS, px) > cw - 2 * PAD - 8)
            px -= 2;                         /* a long number gets smaller */
        ud_text_right(win, cw - PAD - 4, DISP_BASE, big, l.text, UD_SANS, px);
    }

    for (int r = 0; r < 5; r++)
        for (int c = 0; c < 4; c++) {
            const char *k = keys[r][c];
            if (!k[0])
                continue;
            int x = key_x(c), y = key_y(r);
            int w = (r == 4 && c == 0) ? 2 * KEY_W + KEY_GAP : KEY_W;
            char ch0 = k[0];
            int is_op = (ch0 == '/' || ch0 == '*' || ch0 == '-' ||
                         (ch0 == '+' && !k[1]) || ch0 == '=');
            int is_fn = r == 0 && c < 3;
            uint32_t face = is_op ? l.accent : is_fn ? l.line : l.row;
            uint32_t ink = is_op ? l.on_accent : l.text;
            /* the operation waiting for its number stays lit */
            if (is_op && op == ch0 && !entering && ch0 != '=') {
                face = l.on_accent;
                ink = l.accent;
            }
            ud_round(win, x, y, w, KEY_H, KEY_H / 2, face);
            const char *label = k;
            if (k[0] == '.' && !k[1])
                label = ulocale_decimal_comma() ? "," : ".";
            if (is_op && ch0 != '+' && ch0 != '=')
                label = op_sign(ch0);
            else if (k[0] == '+' && k[1] == '/')
                label = "\xC2\xB1";              /* plus-minus */
            if (ch0 == '-' && !k[1]) {           /* minus: as long as plus */
                int mx = x + w / 2, my = y + KEY_H / 2;
                ud_line(win, mx - 6, my, mx + 6, my, 30, ink);
            } else {
                ud_text_mid(win, x, y, w, KEY_H, label, ink, UD_SANS, 18);
            }
        }
}

/* Which key is at (x, y), or 0 */
static const char *key_at(int x, int y)
{
    for (int r = 0; r < 5; r++)
        for (int c = 0; c < 4; c++) {
            const char *k = keys[r][c];
            if (!k[0])
                continue;
            int w = (r == 4 && c == 0) ? 2 * KEY_W + KEY_GAP : KEY_W;
            if (x >= key_x(c) && x < key_x(c) + w && y >= key_y(r) &&
                y < key_y(r) + KEY_H)
                return k;
        }
    return (const char *)0;
}

void _start(void)
{
    int win = (int)win_create(W0, H0, ulocale_language_polish()
                                          ? "Kalkulator" : "Calculator");
    if (win < 0)
        sys_exit(1);
    int cw = W0, ch = H0;
    draw(win, cw, ch);

    struct ud_look was;
    ud_look(&was);
    int language = ulocale_language_polish();
    for (;;) {
        int again = 0;
        int current_language = ulocale_language_polish();
        if (current_language != language) {
            language = current_language;
            again = 1;
        }
        struct win_event ev;
        while (win_poll(win, &ev) == 1) {
            if (ev.type == WEV_CLOSE)
                sys_exit(0);
            if (ev.type == WEV_SIZE) {
                cw = ev.x;
                ch = ev.y;
            } else if (ev.type == WEV_CLICK) {
                const char *k = key_at(ev.x, ev.y);
                if (k)
                    press(k);
            } else if (ev.type == WEV_KEY) {
                char c = (char)ev.val;
                char k[4] = { c, 0, 0, 0 };
                if (c == '\n' || c == '\r')
                    k[0] = '=';
                else if (c == '\b')
                    k[0] = '<';
                else if (c == 27 || c == 'c')
                    k[0] = 'C';
                else if (c == ',')
                    k[0] = '.';
                else if (c == 'n') {             /* the sign */
                    k[0] = '+';
                    k[1] = '/';
                    k[2] = '-';
                }
                press(k);
            }
            again = 1;
        }
        struct ud_look now;
        ud_look(&now);
        if (now.dark != was.dark) {
            was = now;
            again = 1;
        }
        if (again)
            draw(win, cw, ch);
        sys_sleep(50);
    }
}
