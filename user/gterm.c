/* gterm - a terminal in a desktop window (ring 3)
 *
 * What was here before ran one command and drew what it printed. Every
 * command wiped out the one before it, the window was 240x150 whatever
 * the person did with it, there was no cursor, no history, no way back
 * to what had scrolled past and no way to change directory. It proved
 * that a program in ring 3 could start another one and read its output.
 * It was not a terminal.
 *
 * A terminal is a session. What has been run stays on the screen above
 * what is being typed, the prompt says where you are, the arrows go
 * back to what you typed before, and the window's size decides how much
 * of it you see - not a number written into the program.
 *
 * So this one keeps a ring of lines, echoes the prompt and the command
 * into it the way a real one does, appends the output as it is read,
 * and draws the tail of it above the line being edited. PgUp/PgDn walk
 * back through what has scrolled by. Left, right, Home and End move
 * inside the line, and typing inserts where the cursor is. 'cd' changes
 * the directory - and the prompt says so at once, which is the whole
 * point of a prompt.
 *
 * There is exactly one thing here that depends on the processor mode:
 * how a command is started and its output captured. In protected mode
 * the kernel has a syscall that does it (spawn with a redirect, and a
 * whole pipeline in one call); in long mode there is fork, dup2, exec
 * and pipe, and the terminal builds it out of those itself. Everything
 * else - the ring of lines, the editing, the history, the drawing - is
 * one piece of code compiled twice.
 */

#include "uapi.h"

/* A dark terminal, the way a terminal looks: the background is the
   palette's black and the text is light. The desktop's own windows are
   grey, so the terminal stands out as what it is - a screen inside a
   window, not a form. */
#define C_BLACK  0
#define C_RED    1
#define C_GREEN  2
#define C_BLUE   3
#define C_WHITE  4
#define C_YELLOW 5
#define C_BG     C_BLACK      /* the terminal's own background */
#define C_TEXT   C_WHITE
#define C_PROMPT C_GREEN
#define C_NOTE   C_YELLOW

/* The size the window opens with. From then on it is whatever the
   person pulled it to, and the desktop says so with WEV_SIZE. */
#define W0 400
#define H0 240

#define COLS_MAX 96           /* the widest line we keep */
#define ROWS_MAX 48
#define SCROLL   140          /* how many lines back you can look */
#define LINE_MAX 120
#define HIST_N   12
#define ROW_H    10
#define PAD      4
#define OUTFILE  "termout"

/* The screen buffer: a ring, because a terminal always throws the
   oldest line away and never the newest. Moving fourteen kilobytes up
   by one line on every newline would be the other way of doing it. */
static char lines[SCROLL][COLS_MAX + 1];
static int  line_len[SCROLL];
static int  first;            /* where the oldest line sits */
static int  count;            /* how many lines the ring holds */
static int  view;             /* how many lines up from the bottom we look */

static char input[LINE_MAX];
static int  ilen, icur;
static char hist[HIST_N][LINE_MAX];
static int  hist_n;            /* how many commands are remembered */
static int  hist_pos;          /* where we are walking through them */

static char prompt[48];
static int  plen;

/* Asking for the password: the next line typed is not a command but a
   password, and the command it is for waits here. Nothing else about
   the terminal changes - the same buffer, the same editing; only what
   is drawn and what is done with it. */
static int  asking;
static char pending[LINE_MAX];
static int  cw = W0, ch = H0;  /* the content area, as it is now */
static int  cols = 48, rows = 23;

/* --- small helpers ------------------------------------------------- */

static int t_len(const char *s)
{
    int n = 0;
    while (s[n])
        n++;
    return n;
}

static int t_eq(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static int t_starts(const char *s, const char *p)
{
    while (*p)
        if (*s++ != *p++)
            return 0;
    return 1;
}

static void t_copy(char *dst, const char *src, int max)
{
    int i = 0;
    while (src[i] && i < max - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

/* --- the ring of lines --------------------------------------------- */

static int at(int i) { return (first + i) % SCROLL; }

static void new_line(void)
{
    if (count == SCROLL) {         /* full - the oldest one goes */
        first = (first + 1) % SCROLL;
        count--;
    }
    int k = (first + count) % SCROLL;
    lines[k][0] = 0;
    line_len[k] = 0;
    count++;
}

static void put_char(char c)
{
    if (count == 0)
        new_line();
    if (c == '\r')
        return;
    if (c == '\n') {
        new_line();
        return;
    }
    if (c == '\t') {
        do
            put_char(' ');
        while (line_len[at(count - 1)] % 8);
        return;
    }
    if (c < 0x20 || c > 0x7E)
        c = '.';                   /* whatever it was, it is not text */

    int k = at(count - 1);
    /* Wrapping happens as the text arrives, at the width the window has
       then. Making the window wider afterwards does not re-flow what is
       already written - the same as in a terminal that has scrolled. */
    if (line_len[k] >= cols || line_len[k] >= COLS_MAX) {
        new_line();
        k = at(count - 1);
    }
    lines[k][line_len[k]++] = c;
    lines[k][line_len[k]] = 0;
}

static void put_str(const char *s)
{
    while (*s)
        put_char(*s++);
}

static void put_num(int32_t v)
{
    char buf[12];
    int n = 0;
    if (v < 0) {
        put_char('-');
        v = -v;
    }
    do {
        buf[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (n)
        put_char(buf[--n]);
}

/* --- the one thing that depends on the processor mode --------------- */

#ifdef __x86_64__

/* Long mode: fork, dup2, exec, wait - the terminal builds the
   redirection itself, because the kernel has no call that does all of
   it in one go. */

#define ARGS_MAX 8
static char words[ARGS_MAX][32];
static char *argv[ARGS_MAX + 1];

static void split(char *text)
{
    int n = 0, i = 0;
    while (text[i] && n < ARGS_MAX) {
        while (text[i] == ' ')
            i++;
        if (!text[i])
            break;
        int k = 0;
        while (text[i] && text[i] != ' ' && k < 31)
            words[n][k++] = text[i++];
        words[n][k] = 0;
        while (text[i] && text[i] != ' ')
            i++;
        argv[n] = words[n];
        n++;
    }
    argv[n] = 0;
}

static void term_cwd(char *buf, int len)
{
    sys_getcwd(buf, (uint64_t)len);
}

/* Opens the output file from scratch: a shorter result must not leave
   the tail of a longer one behind it. */
static int out_file(void)
{
    sys_unlink(OUTFILE);
    return (int)sys_open(OUTFILE, O_CREATE | O_WRITE);
}

static int32_t run_one(char *cmd)
{
    int fd = out_file();
    if (fd < 0)
        return -2;

    int64_t pid = sys_fork();
    if (pid == 0) {
        sys_dup2(fd, FD_STDOUT);
        sys_close(fd);
        split(cmd);
        sys_exec(argv[0], argv);
        sys_exit(127);             /* exec comes back only on an error */
    }
    sys_close(fd);
    if (pid < 0)
        return -2;

    int code = 0;
    sys_wait(&code);
    /* The child cannot say 'there is no such program' in any other way
       than by dying, so 127 is the agreement between it and us. */
    return code == 127 ? -1 : (int32_t)code;
}

/* 'a | b': the pipe belongs to the terminal, both its ends are handed
   to the children and BOTH are then closed here - otherwise the second
   program would wait for an end of data that never comes. */
static int32_t run_pipe(char *left, char *right)
{
    int fd[2];
    if (sys_pipe(fd) < 0)
        return -2;
    int out = out_file();
    if (out < 0) {
        sys_close(fd[0]);
        sys_close(fd[1]);
        return -2;
    }

    int64_t a = sys_fork();
    if (a == 0) {
        sys_close(fd[0]);
        sys_close(out);
        sys_dup2(fd[1], FD_STDOUT);
        sys_close(fd[1]);
        split(left);
        sys_exec(argv[0], argv);
        sys_exit(127);
    }
    int64_t b = sys_fork();
    if (b == 0) {
        sys_close(fd[1]);
        sys_dup2(fd[0], FD_STDIN);
        sys_close(fd[0]);
        sys_dup2(out, FD_STDOUT);
        sys_close(out);
        split(right);
        sys_exec(argv[0], argv);
        sys_exit(127);
    }
    sys_close(fd[0]);
    sys_close(fd[1]);
    sys_close(out);
    if (a < 0 || b < 0)
        return -2;

    int code = 0, last = 0;
    sys_wait(&code);
    last = code;
    sys_wait(&code);
    /* Whichever of them ended second, the code that matters is the one
       from the right-hand side - and both of ours are these two. */
    if (last == 127 || code == 127)
        return -1;
    return (int32_t)code;
}

#else

/* Protected mode: the kernel starts a program (or a whole pipeline)
   with the output redirected, in one call. */

static void term_cwd(char *buf, int len)
{
    (void)len;
    sys_getcwd(buf);
}

static int32_t run_one(char *cmd)
{
    int32_t pid = sys_spawn(cmd, OUTFILE, 0);
    if (pid < 0)
        return -1;
    return sys_wait(pid);
}

static int32_t run_pipe(char *left, char *right)
{
    /* The kernel takes the pipeline as one line, so we put back the
       '|' we cut it at */
    (void)right;
    left[t_len(left)] = '|';
    int32_t pid = sys_pipeline(left, OUTFILE);
    if (pid < 0)
        return -1;
    return sys_wait(pid);
}

#endif

/* --- running a line ------------------------------------------------- */

static void refresh_prompt(void)
{
    char dir[32];
    dir[0] = 0;
    term_cwd(dir, (int)sizeof(dir));
    if (!dir[0]) {
        dir[0] = '/';
        dir[1] = 0;
    }
    int i = 0;
    while (dir[i] && i < (int)sizeof(prompt) - 3) {
        prompt[i] = dir[i];
        i++;
    }
    /* '#' for an admin and '>' for everybody else - so that what you
       are is on the screen and not something to remember */
    prompt[i++] = sys_level() == LEVEL_ADMIN ? '#' : '>';
    prompt[i++] = ' ';
    prompt[i] = 0;
    plen = i;
}

/* Reads what the command wrote and puts it into the ring - in pieces,
   because the output may be longer than anything we would want to hold
   whole. What does not fit in the ring scrolls away, exactly as it
   would in a terminal. */
static void show_output(void)
{
    int32_t fd = sys_open(OUTFILE, O_READ);
    if (fd < 0)
        return;
    char buf[256];
    for (;;) {
        int32_t n = sys_read((int)fd, buf, sizeof(buf));
        if (n <= 0)
            break;
        for (int32_t i = 0; i < n; i++)
            put_char(buf[i]);
        if (n < (int32_t)sizeof(buf))
            break;
    }
    sys_close((int)fd);
    /* A program whose last line has no newline would leave the next
       prompt stuck to its output */
    if (count && line_len[at(count - 1)])
        put_char('\n');
}

static void remember(const char *cmd)
{
    if (hist_n && t_eq(hist[(hist_n - 1) % HIST_N], cmd))
        return;                    /* the same as the last one */
    t_copy(hist[hist_n % HIST_N], cmd, LINE_MAX);
    hist_n++;
}

/* Runs what was waiting for the password, as an admin, and steps back
   down at once. One command, not a session - the right is given to the
   thing that was asked for and to nothing else. */
static void run_pending(void);
static void run_program(char *line);

static void run_line(void)
{
    char cmd[LINE_MAX];
    input[ilen] = 0;
    t_copy(cmd, input, sizeof(cmd));
    ilen = 0;
    icur = 0;
    input[0] = 0;              /* the line is gone, not merely 'empty' */
    view = 0;

    /* The password: never echoed, never remembered, never put on the
       screen - the line above shows stars and this one shows nothing */
    if (asking) {
        asking = 0;
        put_char('\n');
        if (sys_auth(cmd) != 0) {
            put_str("sudo: wrong password\n");
            pending[0] = 0;
            return;
        }
        run_pending();
        sys_drop();
        refresh_prompt();
        return;
    }

    /* The command goes onto the screen with its prompt, the way it
       happened - so that what is above the cursor is a record of the
       session and not just a pile of output */
    put_str(prompt);
    put_str(cmd);
    put_char('\n');

    int i = 0;
    while (cmd[i] == ' ')
        i++;
    char *line = cmd + i;
    if (!*line)
        return;

    remember(line);
    hist_pos = hist_n;

    if (t_eq(line, "whoami")) {
        put_str(sys_level() == LEVEL_ADMIN
                    ? "admin - you can change the system\n"
                    : "plain user - use 'sudo' to change the system\n");
        return;
    }
    if (t_eq(line, "sudo") || t_starts(line, "sudo ")) {
        const char *rest = line + 4;
        while (*rest == ' ')
            rest++;
        if (!*rest) {
            put_str("sudo: which command?\n");
            return;
        }
        if (sys_level() == LEVEL_ADMIN) {   /* already, so no asking */
            t_copy(pending, rest, LINE_MAX);
            run_pending();
            return;
        }
        t_copy(pending, rest, LINE_MAX);
        asking = 1;
        put_str("password: ");
        return;
    }
    if (t_eq(line, "clear")) {
        first = 0;
        count = 0;
        return;
    }
    if (t_eq(line, "exit")) {
        sys_exit(0);
    }
    if (t_eq(line, "help")) {
        put_str("built in: cd <dir>, clear, exit, help, whoami, "
                "sudo <command>\n");
        put_str("everything else is a program from the disk or the "
                "initrd; 'a | b' works\n");
        put_str("arrows: history and moving in the line; "
                "PgUp/PgDn: scrolling\n");
        return;
    }
    if (t_eq(line, "cd") || t_starts(line, "cd ")) {
        const char *path = line + 2;
        while (*path == ' ')
            path++;
        if (!*path)
            path = "/";
        if (sys_chdir(path) < 0) {
            put_str("cd: no such directory: ");
            put_str(path);
            put_char('\n');
        } else {
            refresh_prompt();
        }
        return;
    }

    run_program(line);
}

/* Starts a program (or two joined by a pipe) and shows what it wrote.
   Kept apart from the line above it because 'sudo' runs a command that
   was typed a moment earlier, not the line that has just been read. */
static void run_program(char *line)
{
    /* 'a | b' - one pipe, which is what a person types */
    char *bar = (char *)0;
    for (char *s = line; *s; s++)
        if (*s == '|') {
            bar = s;
            break;
        }

    int32_t code;
    if (bar) {
        *bar = 0;
        char *right = bar + 1;
        while (*right == ' ')
            right++;
        code = run_pipe(line, right);
    } else {
        code = run_one(line);
    }

    if (code == -1) {
        put_str("no such program: ");
        put_str(line);
        put_char('\n');
        return;
    }
    if (code == -2) {
        put_str("could not start it\n");
        return;
    }

    show_output();
    /* A code other than zero is not an error of ours, but it is
       something the person has to be able to see */
    if (code) {
        put_str("[code ");
        put_num(code);
        put_str("]\n");
    }
}

/* What was waiting for the password. It is echoed here rather than
   when it was typed, so that the record reads in the order things
   happened: the ask, then the command, then its output. */
static void run_pending(void)
{
    if (!pending[0])
        return;
    put_str(prompt);
    put_str("sudo ");
    put_str(pending);
    put_char('\n');
    run_program(pending);
    pending[0] = 0;
}

/* --- keys ----------------------------------------------------------- */

static void take_history(int which)
{
    if (which < 0 || which >= hist_n) {
        ilen = 0;
        icur = 0;
        input[0] = 0;
        return;
    }
    t_copy(input, hist[which % HIST_N], LINE_MAX);
    ilen = t_len(input);
    icur = ilen;
}

static void on_key(char c)
{
    int text_rows = rows - 1;

    switch ((unsigned char)c) {
    case KEY_UP:
        if (hist_pos > 0 && hist_n) {
            hist_pos--;
            take_history(hist_pos);
        }
        return;
    case KEY_DOWN:
        if (hist_pos < hist_n) {
            hist_pos++;
            take_history(hist_pos);
        }
        return;
    case KEY_LEFT:
        if (icur > 0)
            icur--;
        return;
    case KEY_RIGHT:
        if (icur < ilen)
            icur++;
        return;
    case KEY_HOME:
        icur = 0;
        return;
    case KEY_END:
        icur = ilen;
        return;
    case KEY_PGUP:
        view += text_rows / 2 ? text_rows / 2 : 1;
        if (view > count - text_rows)
            view = count - text_rows;
        if (view < 0)
            view = 0;
        return;
    case KEY_PGDN:
        view -= text_rows / 2 ? text_rows / 2 : 1;
        if (view < 0)
            view = 0;
        return;
    }

    if (c == '\n' || c == '\r') {
        run_line();
        return;
    }
    if (c == '\b') {
        if (icur > 0) {
            for (int i = icur - 1; i < ilen - 1; i++)
                input[i] = input[i + 1];
            ilen--;
            icur--;
            input[ilen] = 0;
        }
        return;
    }
    if (c >= 0x20 && c < 0x7F && ilen < LINE_MAX - 1) {
        /* Typing puts the character WHERE THE CURSOR IS, not at the end
           - otherwise moving the cursor would be a thing you can do and
           not a thing that means anything */
        for (int i = ilen; i > icur; i--)
            input[i] = input[i - 1];
        input[icur++] = c;
        ilen++;
        input[ilen] = 0;
        view = 0;
    }
}

/* --- drawing -------------------------------------------------------- */

static void measure(void)
{
    cols = (cw - 2 * PAD) / 8;
    if (cols < 8)
        cols = 8;
    if (cols > COLS_MAX)
        cols = COLS_MAX;
    rows = (ch - 2 * PAD) / ROW_H;
    if (rows < 2)
        rows = 2;
    if (rows > ROWS_MAX)
        rows = ROWS_MAX;
}

static void draw(int w)
{
    char buf[COLS_MAX + 1];
    int text_rows = rows - 1;

    win_rect(w, 0, 0, cw, ch, C_BG);

    int last = count - view;
    if (last < 0)
        last = 0;
    int from = last - text_rows;
    if (from < 0)
        from = 0;

    int y = PAD;
    for (int i = from; i < last; i++) {
        t_copy(buf, lines[at(i)], cols + 1);
        win_text(w, PAD, y, buf, C_TEXT);
        y += ROW_H;
    }

    /* The line being edited always sits at the bottom, whether or not
       there is enough above it to fill the window */
    int iy = PAD + text_rows * ROW_H;
    /* While a password is being typed the line says so instead of
       saying where you are: what is being asked for matters more than
       which directory it happens in. */
    const char *lead = asking ? "password: " : prompt;
    int leadn = asking ? 10 : plen;
    win_text(w, PAD, iy, lead, C_PROMPT);

    /* A line longer than the window slides under the cursor, so the
       cursor is always somewhere a person can see it */
    int room = cols - leadn;
    if (room < 4)
        room = 4;
    int off = icur >= room ? icur - room + 1 : 0;
    t_copy(buf, input + off, room + 1);
    /* A password is typed in the same line as everything else and shown
       in none of it: stars are what a person needs - that the keys are
       arriving - and all they need. */
    if (asking)
        for (int i = 0; buf[i]; i++)
            buf[i] = '*';
    win_text(w, PAD + leadn * 8, iy, buf, C_TEXT);

    /* The cursor: a line under the character it stands on. It does not
       blink - a blinking cursor is a nicety, and one that is on half
       the time is one that can be photographed absent. */
    win_rect(w, PAD + (leadn + icur - off) * 8, iy + ROW_H - 2, 8, 2,
             C_PROMPT);

    /* Scrolled up, the last thing on the screen is not the last thing
       that happened - and that has to be said, or the terminal looks
       stuck */
    if (view && cols > 24) {
        /* Cleared first: drawn straight onto the line underneath, the
           two texts would share the same pixels and neither would be
           readable. */
        int mx = PAD + (cols - 15) * 8;      /* "^ scrolled back" */
        win_rect(w, mx, PAD, 15 * 8, 8, C_BG);
        win_text(w, mx, PAD, "^ scrolled back", C_NOTE);
    }
}

void _start(void)
{
    int w = (int)win_create(W0, H0, "Terminal");
    if (w < 0)
        sys_exit(1);

    measure();
    refresh_prompt();
    put_str("ProtonOS terminal - type 'help' to see what it can do.\n");

    for (;;) {
        struct win_event ev;
        while (win_poll(w, &ev) == 1) {
            if (ev.type == WEV_CLOSE)
                sys_exit(0);
            if (ev.type == WEV_SIZE) {
                cw = ev.x;
                ch = ev.y;
                measure();
            } else if (ev.type == WEV_KEY) {
                on_key((char)ev.val);
            }
        }

        draw(w);
        sys_sleep(50);
    }
}
