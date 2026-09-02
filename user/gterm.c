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
static int  term_win = -1;     /* the window, so the prompt can rename it */
static int  plen;

/* Asking for the password: the next line typed is not a command but a
   password, and the command it is for waits here. Nothing else about
   the terminal changes - the same buffer, the same editing; only what
   is drawn and what is done with it. */
static int  asking;
static char pending[LINE_MAX];

/* A program that is running RIGHT NOW.
 *
 * It used to be started and waited for, and the window did nothing at
 * all in the meantime: it could not be moved, could not be closed, and
 * showed nothing until the program had ended. Which meant that a
 * program that does not end - and 'spin' is one - took the terminal
 * with it.
 *
 * Now the terminal starts it, keeps drawing, and reads what it has
 * written so far on every pass. 'shown' is how much of that output has
 * already been put on the screen, so that reading again brings only
 * what is new. */
static int32_t running;
static int32_t helper;         /* the other half of a pipe, 0 = none */
static uint32_t shown;
/* The way IN to the running program: the write end of the pipe that is
   its stdin. What is typed while it runs goes there, which is the
   whole difference between running a program and talking to one. -1
   when there is nobody to talk to. */
static int in_fd = -1;

/* Selecting with the mouse.
 *
 * A selection is two places in the ring of lines - not two places on
 * the screen: what is selected stays selected when the view scrolls
 * under it. 'sel_line' is an index into the ring (0 = the oldest line
 * kept), 'sel_col' a column in that line.
 *
 * Letting the button go copies what is between them, the way it works
 * in a terminal that has had a mouse for forty years: selecting IS
 * copying, and there is nothing else to press. */
static int sel_on;             /* is there a selection at all */
static int sel_drag;           /* is the button still down */
static int sel_line0, sel_col0;   /* where it started */
static int sel_line1, sel_col1;   /* where it is now */
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

/* Starts the program and comes straight back with its pid. Whether
   there is such a program at all is not known yet: in long mode that
   answer comes from the child, as the exit code 127 of a failed exec.
   So it arrives later, where the exit code does. */
static int32_t start_one(char *cmd)
{
    int fd = out_file();
    if (fd < 0)
        return -2;

    /* A pipe for what the person types: the child reads its end as
       descriptor zero and knows nothing about where the bytes come
       from - which is exactly how a terminal is supposed to look from
       inside a program. */
    int pin[2];
    if (sys_pipe(pin) < 0) {
        sys_close(fd);
        return -2;
    }

    int64_t pid = sys_fork();
    if (pid == 0) {
        sys_dup2(fd, FD_STDOUT);
        sys_close(fd);
        sys_dup2(pin[0], FD_STDIN);
        sys_close(pin[0]);
        sys_close(pin[1]);         /* the child must not hold OUR end */
        split(cmd);
        sys_exec(argv[0], argv);
        sys_exit(127);             /* exec comes back only on an error */
    }
    sys_close(fd);
    sys_close(pin[0]);
    if (pid < 0) {
        sys_close(pin[1]);
        return -2;
    }
    in_fd = pin[1];
    return (int32_t)pid;
}

/* 'a | b': the pipe belongs to the terminal, both its ends are handed
   to the children and BOTH are then closed here - otherwise the second
   program would wait for an end of data that never comes. */
static int32_t start_pipe(char *left, char *right)
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

    /* The right-hand side is the one whose output is shown, so it is
       the one we watch. The left one is collected when it is over -
       by then it has nothing left to do. */
    helper = (int32_t)a;
    return (int32_t)b;
}

#else

/* Protected mode: the kernel starts a program (or a whole pipeline)
   with the output redirected, in one call. */

static void term_cwd(char *buf, int len)
{
    (void)len;
    sys_getcwd(buf);
}

/* The output file, emptied before anything writes into it.
 *
 * Whoever reads it while it grows has to be sure that what is in it
 * came from THIS command: the terminal shows what is past the point it
 * last read, and a file still holding the last program's output would
 * be shown as this one's. */
static void empty_out_file(void)
{
    int32_t fd = sys_open(OUTFILE, O_WRITE | O_CREATE | O_TRUNC);
    if (fd >= 0)
        sys_close((int)fd);
}

/* Starts the program with its output going to a file and comes back
   at once: the pid, or -1 when there is no such program (which this
   kernel knows straight away, because it is the one that looks for
   it). */
static int32_t start_one(char *cmd)
{
    empty_out_file();
    int fd = -1;
    int32_t pid = sys_spawn_talk(cmd, OUTFILE, &fd);
    if (pid < 0)
        return -1;
    in_fd = fd;                    /* the way in to what we started */
    return pid;
}

static int32_t start_pipe(char *left, char *right)
{
    /* The kernel takes the pipeline as one line, so we put back the
       '|' we cut it at */
    (void)right;
    empty_out_file();
    left[t_len(left)] = '|';
    int32_t pid = sys_pipeline(left, OUTFILE);
    return pid < 0 ? -1 : pid;
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

    /* And the window says where it is standing. Two terminals in two
       directories are two windows called "Terminal" otherwise - and it
       is the taskbar, which shows this name under the pointer, that
       has to tell them apart. */
    char t[48];
    int n = 0;
    const char *pre = "Terminal: ";
    for (int k = 0; pre[k]; k++)
        t[n++] = pre[k];
    for (int k = 0; dir[k] && n < (int)sizeof(t) - 1; k++)
        t[n++] = dir[k];
    t[n] = 0;
    if (term_win >= 0)
        win_title(term_win, t);
}

/* Shows whatever the running program has written since the last look.
 *
 * The output goes to a file, so 'what is new' is simply 'what is past
 * the point we stopped at' - we seek there and read to the end. Called
 * on every pass of the main loop, so a program that prints as it goes
 * is seen printing as it goes, rather than all at once when it is
 * over.
 *
 * What does not fit in the ring of lines scrolls away, exactly as it
 * would in a terminal. */
static void show_new_output(void)
{
    int32_t fd = sys_open(OUTFILE, O_READ);
    if (fd < 0)
        return;
    if (shown)
        sys_seek((int)fd, shown);
    char buf[256];
    for (;;) {
        int32_t n = sys_read((int)fd, buf, sizeof(buf));
        if (n <= 0)
            break;
        for (int32_t i = 0; i < n; i++)
            put_char(buf[i]);
        shown += (uint32_t)n;
        if (n < (int32_t)sizeof(buf))
            break;
    }
    sys_close((int)fd);
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
static void watch_running(void);

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

    int32_t pid;
    helper = 0;
    shown = 0;
    if (bar) {
        *bar = 0;
        char *right = bar + 1;
        while (*right == ' ')
            right++;
        pid = start_pipe(line, right);
    } else {
        pid = start_one(line);
    }

    if (pid == -1) {
        put_str("no such program: ");
        put_str(line);
        put_char('\n');
        return;
    }
    if (pid < 0) {
        put_str("could not start it\n");
        return;
    }

    /* From here the terminal goes back to drawing. The program writes,
       we read what is new on every pass, and the end of it is noticed
       in the same place - see watch_running(). */
    running = pid;
}

/* Is the program still running, and what has it written? Called on
   every pass of the main loop, which is what makes the difference
   between a window that has frozen and a window that is waiting. */
static void watch_running(void)
{
    int code = 0;
    int32_t rc = sys_trywait(running, &code);

    /* Read AFTER asking whether it has ended: anything written between
       the two is then still caught by the final read below. */
    show_new_output();
    if (rc == -3)
        return;                    /* still going */

    show_new_output();             /* the last of it */
    running = 0;
    if (in_fd >= 0) {
        sys_close(in_fd);          /* nobody left to talk to */
        in_fd = -1;
    }

    /* The other half of a pipe has nothing left to do once the right
       side is over; collecting it keeps the process table clean. */
    if (helper) {
        int hc = 0;
        for (int i = 0; i < 50 && sys_trywait(helper, &hc) == -3; i++)
            sys_sleep(10);
        helper = 0;
    }

    /* A program whose last line has no newline would leave the next
       prompt stuck to its output */
    if (count && line_len[at(count - 1)])
        put_char('\n');

    /* In long mode a failed exec can only come back as the child's
       exit code, so 'there is no such program' arrives here */
    if (code == 127) {
        put_str("no such program\n");
        return;
    }
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

    /* Ctrl+C. The keyboard driver turns it into 0x03 and the desktop
       hands it to whoever has the focus, so it arrives here like any
       other key - and here it means the one thing it means everywhere:
       stop that. A program with no handler of its own dies on it. */
    if (c == 0x03) {
        if (running) {
            put_str("^C\n");
            sys_kill((int)running, SIGINT);
        }
        return;
    }
    if (c == '\n' || c == '\r') {
        /* While a program runs, a line is not a command - it is what
           the program is being told. It goes down the pipe and onto
           the screen, because a terminal shows what was typed into it
           whether a program is reading it or not. */
        if (running) {
            if (in_fd >= 0) {
                input[ilen] = 0;
                put_str(input);
                put_char('\n');
                input[ilen++] = '\n';
                sys_write(in_fd, input, (uint32_t)ilen);
            }
            ilen = 0;
            icur = 0;
            input[0] = 0;
            view = 0;
            return;
        }
        run_line();
        return;
    }
    /* The clipboard. Ctrl+Insert takes the line being typed, and
       Shift+Insert puts what is there into it - at the cursor, like
       any other typing. The text goes through the kernel, so the
       other end of it can be any other window. */
    if (c == 0x1A) {                    /* KEY_COPY */
        input[ilen] = 0;
        if (ilen)
            sys_clip_set(input, (uint32_t)ilen);
        return;
    }
    if (c == 0x1B) {                    /* KEY_PASTE */
        char got[CLIP_MAX + 1];
        int n = (int)sys_clip_get(got, sizeof(got));
        for (int i = 0; i < n && ilen < LINE_MAX - 1; i++) {
            char ch = got[i];
            if (ch < 0x20 || ch > 0x7E)
                continue;               /* one line, so no newlines */
            for (int k = ilen; k > icur; k--)
                input[k] = input[k - 1];
            input[icur++] = ch;
            ilen++;
        }
        input[ilen] = 0;
        view = 0;
        return;
    }
    /* Ctrl+D: no more input. The pipe closes and whoever was reading
       sees the end of it - which for a shell means 'that is all'. */
    if (c == 0x04) {
        if (running && in_fd >= 0) {
            put_str("^D\n");
            sys_close(in_fd);
            in_fd = -1;
        }
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

/* --- selecting with the mouse --------------------------------------- */

/* The first line of the view: what the drawing starts from, and what a
   pointer position has to be measured against. */
static int view_first(int text_rows)
{
    int last = count - view;
    if (last < 0)
        last = 0;
    int from = last - text_rows;
    return from < 0 ? 0 : from;
}

/* A place in the window turned into a place in the ring of lines. */
static void where(int x, int y, int *line, int *col)
{
    int text_rows = rows - 1;
    int r = (y - PAD) / ROW_H;
    if (r < 0)
        r = 0;
    if (r > text_rows - 1)
        r = text_rows - 1;
    int c = (x - PAD + 4) / 8;          /* rounded to the nearest gap */
    if (c < 0)
        c = 0;
    if (c > cols)
        c = cols;
    *line = view_first(text_rows) + r;
    *col = c;
}

/* Is 'a' before 'b'? A selection made upwards is the same selection as
   one made downwards - it is a range, and a range has no direction. */
static int before(int la, int ca, int lb, int cb)
{
    return la < lb || (la == lb && ca <= cb);
}

static void sel_range(int *la, int *ca, int *lb, int *cb)
{
    if (before(sel_line0, sel_col0, sel_line1, sel_col1)) {
        *la = sel_line0; *ca = sel_col0;
        *lb = sel_line1; *cb = sel_col1;
    } else {
        *la = sel_line1; *ca = sel_col1;
        *lb = sel_line0; *cb = sel_col0;
    }
}

/* Puts what is selected into the clipboard - the kernel's, so it can
   be pasted into any other window. */
static void sel_copy(void)
{
    if (!sel_on)
        return;
    int la, ca, lb, cb;
    sel_range(&la, &ca, &lb, &cb);
    if (la == lb && ca == cb)
        return;                        /* a click, not a selection */

    char out[CLIP_MAX];
    int n = 0;
    for (int i = la; i <= lb && i < count && n < CLIP_MAX; i++) {
        const char *src = lines[at(i)];
        int len = line_len[at(i)];
        int from = (i == la) ? ca : 0;
        int to = (i == lb) ? cb : len;
        if (from > len)
            from = len;
        if (to > len)
            to = len;
        for (int k = from; k < to && n < CLIP_MAX; k++)
            out[n++] = src[k];
        if (i != lb && n < CLIP_MAX)
            out[n++] = '\n';
    }
    if (n)
        sys_clip_set(out, (uint32_t)n);
}

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

    int la = 0, ca = 0, lb = 0, cb = 0;
    if (sel_on)
        sel_range(&la, &ca, &lb, &cb);

    int y = PAD;
    for (int i = from; i < last; i++) {
        /* What is selected gets a field behind it, drawn before the
           text - win_text puts down the glyph's own pixels and nothing
           else, so the field shows through around them. */
        if (sel_on && i >= la && i <= lb) {
            int len = line_len[at(i)];
            int f = (i == la) ? ca : 0;
            int t = (i == lb) ? cb : len;
            if (f > len) f = len;
            if (t > len) t = len;
            if (t > f)
                win_rect(w, PAD + f * 8, y - 1, (t - f) * 8, ROW_H - 1,
                         C_BLUE);
        }
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
    /* A program is running: say so, and say how to stop it. Without
       this the terminal looks the same whether it is waiting for a
       command or waiting for a program, which are not the same thing
       to be looking at. */
    if (running && cols > 24) {
        int mx = PAD + (cols - 15) * 8;
        win_rect(w, mx, PAD, 15 * 8, 8, C_BG);
        win_text(w, mx, PAD, "running... ^C", C_NOTE);
    } else if (view && cols > 24) {
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
    term_win = w;
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
            if (ev.type == WEV_PRESS) {
                where(ev.x, ev.y, &sel_line0, &sel_col0);
                sel_line1 = sel_line0;
                sel_col1 = sel_col0;
                sel_on = 1;
                sel_drag = 1;
            } else if (ev.type == WEV_DRAG) {
                if (sel_drag)
                    where(ev.x, ev.y, &sel_line1, &sel_col1);
            } else if (ev.type == WEV_RELEASE) {
                if (sel_drag) {
                    where(ev.x, ev.y, &sel_line1, &sel_col1);
                    sel_drag = 0;
                    /* Selecting is copying - there is no second key to
                       press, and never was in a terminal */
                    sel_copy();
                    if (sel_line0 == sel_line1 && sel_col0 == sel_col1)
                        sel_on = 0;     /* a click clears it instead */
                }
            } else if (ev.type == WEV_SIZE) {
                cw = ev.x;
                ch = ev.y;
                measure();
            } else if (ev.type == WEV_KEY) {
                on_key((char)ev.val);
            }
        }

        if (running)
            watch_running();

        draw(w);
        sys_sleep(50);
    }
}
