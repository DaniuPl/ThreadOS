/* ginstal - installing ProtonOS onto this machine's disk, in a window
 *
 * Installing on a UEFI machine is three files written onto the EFI
 * partition that is already there, and that is a thing better SEEN
 * than typed. What is at stake is somebody else's disk, so the window
 * spends most of its space saying what it would write, where, and -
 * just as important - what it will not touch. Only then is there a
 * button.
 *
 * The program holds no knowledge about any of it. It asks the kernel
 * what the disk looks like and asks the kernel to take one step at a
 * time; deciding what installing means is one place, and it is not
 * here. What the window owns is the part a window is for: showing it,
 * asking, and moving the bar along.
 *
 * Two things are deliberate and would be easy to get wrong.
 *
 * The word. A button is a reflex, and reflexes are how disks get
 * written over; the console version asks for TAK typed out and so does
 * this one. It is one word and it costs a second, and on the other
 * side of that second is a laptop somebody boots tomorrow.
 *
 * The steps, one per turn of the loop. The kernel could write all five
 * files in one call and the window would freeze for the length of it.
 * Asking for one step per turn means the bar moves and the name under
 * it says which file is being written - and a person watching a disk
 * being written to has a right to see where it is.
 *
 * Nothing here depends on the processor mode: one source, both
 * kernels.
 */

#include "uapi.h"

/* Colours, and the one rule they follow: dark text on a light
 * surface, light text on a dark one. The window's background is the
 * system's light grey, so its text is BLACK - white on that grey was
 * the first thing that went wrong here, and it went wrong invisibly:
 * everything was drawn, nothing was readable. The two fields the
 * person types into are the other way round, because they are black.
 */
#define C_BLACK 0
#define C_RED   1     /* red - a warning, on the light surface */
#define C_BLUE  3     /* the system's accent */
#define C_WHITE 4     /* only ever on something dark */
#define C_BG    8     /* the window's light grey */
#define C_DIM   15    /* dark grey - said quietly, still readable */

#define C_TEXT  C_BLACK
#define C_HEAD  C_BLUE

#define W0 440
#define H0 230

#define BTN_W  168
#define BTN_H  16

/* Every line of text sits on the same twelve-pixel grid, from the top
   of the content down. Not for tidiness: the font is eight pixels
   high, and text laid out by eye - one line at twenty, the next at
   fifty - is text that only a person can read. A grid is text a
   program can read too, and a test that reads the window the way a
   person does is worth more than one that asks the program whether it
   thinks it drew something. */
#define ROW_H  12
#define ROW_Y0 4
static int rowy(int r) { return ROW_Y0 + r * ROW_H; }

/* The same five steps the kernel takes, in the same order. The paths
   are shown on the screen above the button anyway - a person who is
   told what will be written should see the same names move past while
   it is. */
static const char *STEP_NAME[INST_STEPS] = {
    "/EFI/ProtonOS",
    "/EFI/ProtonOS/BOOTX64.EFI",
    "/EFI/ProtonOS/protonos64.bin",
    "/EFI/ProtonOS/initrd64.img",
    "/EFI/BOOT/BOOTX64.EFI"
};

/* Taking the whole disk adds three steps in front: a table, a
   filesystem on the EFI partition it makes, and one on ours. */
static const char *STEP_WHOLE[INST_STEPS_WHOLE] = {
    "tablica partycji (GPT)",
    "partycja EFI (FAT32)",
    "partycja ProtonOS (PFS)",
    "/EFI/ProtonOS",
    "/EFI/ProtonOS/BOOTX64.EFI",
    "/EFI/ProtonOS/protonos64.bin",
    "/EFI/ProtonOS/initrd64.img",
    "/EFI/BOOT/BOOTX64.EFI"
};

/* Where the program is in the business. */
#define S_LOOK  0     /* showing what was found */
#define S_WORD  1     /* waiting for the word */
#define S_PASS  2     /* the kernel said no - waiting for a password */
#define S_WORK  3     /* writing, one step per turn */
#define S_DONE  4
#define S_FAIL  5

static struct install_info info;
static int state = S_LOOK;
static int step;                  /* the next step to take */
static int failed_at = -1;
static char typed[12];
static int tlen;
static char pass[32];
static int plen;
static char note[56];

static const char *step_name(int n)
{
    if (info.mode == INST_WHOLE)
        return (n >= 0 && n < INST_STEPS_WHOLE) ? STEP_WHOLE[n]
                                                : STEP_WHOLE[0];
    return (n >= 0 && n < INST_STEPS) ? STEP_NAME[n] : STEP_NAME[0];
}

static int we_authed;             /* we asked for the right, so we give it back */
static int cw = W0, ch = H0;

static void ecopy(char *d, const char *s, int max)
{
    int i = 0;
    while (s[i] && i < max - 1) {
        d[i] = s[i];
        i++;
    }
    d[i] = '\0';
}

static int num(char *d, uint32_t v)
{
    char tmp[12];
    int n = 0;
    do {
        tmp[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v && n < 11);
    for (int i = 0; i < n; i++)
        d[i] = tmp[n - 1 - i];
    d[n] = '\0';
    return n;
}

/* Text, a number, text - the shape of nearly every line here */
static int add(char *d, int at, const char *s)
{
    int i = 0;
    while (s[i])
        d[at++] = s[i++];
    d[at] = '\0';
    return at;
}

static int addnum(char *d, int at, uint32_t v)
{
    return at + num(d + at, v);
}

/* --- what the kernel said ------------------------------------------- */

static const char *problem_line(uint32_t why)
{
    switch (why) {
    case INST_NO_DISK:  return "Nie ma dysku.";
    case INST_NO_FILES: return "Nie ma z czego instalowac.";
    case INST_NO_TABLE: return "Na dysku nie ma tablicy partycji.";
    case INST_NO_ESP:   return "Nie ma partycji EFI.";
    case INST_NOT_FAT:  return "Partycja EFI nie jest FAT-em.";
    case INST_NO_ROOM:  return "Na partycji EFI brakuje miejsca.";
    default:            return "Nie wiadomo, co jest nie tak.";
    }
}

/* A second line, because the first one says WHAT and a person standing
   in front of a machine that will not do something wants to know what
   to do about it. */
static const char *problem_hint(uint32_t why)
{
    switch (why) {
    case INST_NO_FILES:
        return "Uruchom maszyne z pendrive'a przez protoboot (UEFI).";
    case INST_NO_TABLE:
        return "To nie jest dysk, z ktorego startuje maszyna z UEFI.";
    case INST_NO_ESP:
        return "Ten dysk nie ma partycji, na ktorej firmware szuka.";
    case INST_NOT_FAT:
        return "Partycja EFI musi byc FAT-em - ta nim nie jest.";
    case INST_NO_ROOM:
        return "Zrob miejsce na partycji EFI i sprobuj jeszcze raz.";
    default:
        return "";
    }
}

/* --- drawing --------------------------------------------------------- */

/* One button in the middle, or two side by side. Two, because there
   are two ways of installing and a person has to be able to see both
   of them before choosing - a way that is reached by first refusing
   the other one is a way nobody finds. */
#define BTN_GAP 10

static int btn_y(void) { return ch - BTN_H - 6; }

static int btn_x(int which, int of)
{
    if (of == 1)
        return (cw - BTN_W) / 2;
    int span = of * BTN_W + (of - 1) * BTN_GAP;
    return (cw - span) / 2 + which * (BTN_W + BTN_GAP);
}

static int nbuttons = 1;             /* how many the screen drawn last has */

static void one_button(int win, int which, int of, const char *label,
                       int colour)
{
    int x = btn_x(which, of);
    int n = 0;
    while (label[n])
        n++;
    win_rect(win, x, btn_y(), BTN_W, BTN_H, colour);
    /* The button is a dark accent, so its label is the light one -
       the same rule the other way round. Centred by counting
       characters: the font is eight pixels wide and always has been. */
    win_text(win, x + (BTN_W - n * 8) / 2, btn_y() + 4, label, C_WHITE);
}

static void button(int win, const char *label, int colour)
{
    nbuttons = 1;
    one_button(win, 0, 1, label, colour);
}

static void buttons(int win, const char *left, int lcol,
                    const char *right, int rcol)
{
    nbuttons = 2;
    one_button(win, 0, 2, left, lcol);
    one_button(win, 1, 2, right, rcol);
}

/* Which button the click was on: 0 = none, 1 = the first, 2 = the
   second. */
static int in_button(int x, int y)
{
    if (y < btn_y() || y >= btn_y() + BTN_H)
        return 0;
    for (int i = 0; i < nbuttons; i++) {
        int bx = btn_x(i, nbuttons);
        if (x >= bx && x < bx + BTN_W)
            return i + 1;
    }
    return 0;
}

/* Which disk this is about, and - when the machine has more than one
   - that clicking the line changes it. A machine with several disks
   is a machine where saying which one is the whole question, and a
   choice that has to be guessed at is not a choice. */
#define DISK_ROW 2

static void disk_line(int win)
{
    char line[80];
    int at = add(line, 0, "Dysk ");

    at = addnum(line, at, info.disk + 1);
    at = add(line, at, " z ");
    at = addnum(line, at, info.disks);
    at = add(line, at, ": ");
    at = add(line, at, info.disk_name);
    at = add(line, at, ", ");
    at = addnum(line, at, info.disk_mib);
    at = add(line, at, " MiB, partycji: ");
    at = addnum(line, at, info.parts);
    win_text(win, 6, rowy(DISK_ROW), line, C_TEXT);
    if (info.disks > 1)
        win_text(win, 6, rowy(DISK_ROW + 1),
                 "(klik w te linie wybiera nastepny dysk)", C_HEAD);
}

static void draw_plan(int win)
{
    char line[80];
    int at;

    win_text(win, 6, rowy(0), "Instalacja ProtonOS-a na dysk tej maszyny",
             C_HEAD);

    disk_line(win);

    at = add(line, 0, "Partycja EFI: nr ");
    at = addnum(line, at, info.esp_index);
    at = add(line, at, ", ");
    at = add(line, at, info.kind);
    if (info.label[0]) {
        at = add(line, at, " ");
        at = add(line, at, info.label);
    }
    at = add(line, at, ", wolne ");
    at = addnum(line, at, info.free_kib);
    at = add(line, at, " KiB");
    win_text(win, 6, rowy(4), line, C_TEXT);

    win_text(win, 6, rowy(6), "Zapisze:", C_HEAD);

    const uint32_t size3[3] = {info.loader_size, info.kernel_size,
                               info.initrd_size};
    for (int i = 0; i < 3; i++) {
        win_text(win, 14, rowy(7 + i), STEP_NAME[1 + i], C_TEXT);
        int n = num(line, size3[i]);
        add(line, n, " B");
        /* The size is said quietly - the name is what matters. And
           the column sits on the same eight-pixel grid the glyphs do:
           a column at an odd offset can be read by an eye and not by
           a program. */
        win_text(win, 6 + 36 * 8, rowy(7 + i), line, C_DIM);
    }

    if (info.fallback_free) {
        win_text(win, 14, rowy(10), STEP_NAME[4], C_TEXT);
        win_text(win, 14, rowy(11), "wolny - maszyna wystartuje sama",
                 C_HEAD);
    } else {
        win_text(win, 14, rowy(10), STEP_NAME[4], C_RED);
        win_text(win, 14, rowy(11), "zajety przez kogos - nie ruszam go",
                 C_RED);
    }

    win_text(win, 6, rowy(13), "NIE ruszam tablicy partycji, innych "
             "partycji", C_TEXT);
    win_text(win, 6, rowy(14), "ani zadnego pliku, ktory juz tam jest.",
             C_TEXT);
    if (!info.fallback_free)
        win_text(win, 6, rowy(15),
                 "Potem dodaj wpis w firmwarze - patrz wyzej.", C_TEXT);
}

/* The other way, which destroys things. The screen says what will be
   gone before it says what will be made - in that order, because that
   is the order in which a person needs to read it. */
static void draw_wipe(int win)
{
    char line[80];
    int at;

    win_text(win, 6, rowy(0), "Caly dysk dla ProtonOS-a", C_RED);

    disk_line(win);

    win_text(win, 6, rowy(5), "TO SKASUJE WSZYSTKO NA TYM DYSKU:", C_RED);
    at = add(line, 0, "tablice partycji, wszystkie ");
    at = addnum(line, at, info.parts);
    at = add(line, at, " partycje i to, co na nich jest.");
    win_text(win, 14, rowy(6), line, C_TEXT);

    win_text(win, 6, rowy(8), "Zrobie:", C_HEAD);
    win_text(win, 14, rowy(9), STEP_WHOLE[0], C_TEXT);
    at = add(line, 0, STEP_WHOLE[1]);
    at = add(line, at, ", ");
    at = addnum(line, at, info.free_kib / 1024);
    at = add(line, at, " MiB");
    win_text(win, 14, rowy(10), line, C_TEXT);
    win_text(win, 14, rowy(11), STEP_WHOLE[2], C_TEXT);
    win_text(win, 14, rowy(12), "i zapisze na niej system.", C_TEXT);

    win_text(win, 6, rowy(14),
             "Innego dysku to nie dotyczy - tylko tego jednego.", C_TEXT);
}

static void draw_bar(int win, int done, int total)
{
    int x = 20, y = ch - BTN_H - 28, w = cw - 40, h = 14;
    win_rect(win, x, y, w, h, C_BLACK);
    if (total > 0 && done > 0) {
        int fill = w * done / total;
        if (fill > w)
            fill = w;
        win_rect(win, x, y, fill, h, C_HEAD);
    }
}

static void draw(int win)
{
    char line[80];

    win_rect(win, 0, 0, cw, ch, C_BG);

    if (state == S_LOOK && !info.ok) {
        win_text(win, 6, rowy(0), "Instalacja ProtonOS-a", C_HEAD);
        win_text(win, 6, rowy(2), problem_line(info.problem), C_RED);
        win_text(win, 6, rowy(3), problem_hint(info.problem), C_TEXT);
        if (info.problem == INST_NO_ROOM) {
            int at = add(line, 0, "wolne ");
            at = addnum(line, at, info.free_kib);
            at = add(line, at, " KiB, trzeba ");
            at = addnum(line, at, info.need_kib);
            at = add(line, at, " KiB");
            win_text(win, 6, rowy(5), line, C_TEXT);
        }
        /* There may be no way to install BESIDE a system and still a
           way to install instead of one. Offering it here is the
           difference between a machine that cannot be installed on
           and one that can, if somebody means it. */
        if (info.can_wipe && info.mode != INST_WHOLE) {
            win_text(win, 6, rowy(7),
                     "Mozna tez wziac caly dysk - skasuje to wszystko,",
                     C_TEXT);
            win_text(win, 6, rowy(8), "co na nim jest.", C_TEXT);
            buttons(win, "Sprawdz jeszcze raz", C_BLUE,
                    "Caly dysk", C_RED);
        } else {
            button(win, "Sprawdz jeszcze raz", C_BLUE);
        }
        return;
    }

    if (state == S_LOOK && info.mode == INST_WHOLE) {
        draw_wipe(win);
        buttons(win, "Wroc", C_BLUE, "Skasuj i zainstaluj", C_RED);
        return;
    }

    if (state == S_LOOK) {
        draw_plan(win);
        if (info.can_wipe)
            buttons(win, "Zainstaluj obok", C_BLUE, "Caly dysk", C_RED);
        else
            button(win, "Zainstaluj", C_BLUE);
        return;
    }

    if (state == S_WORD) {
        int wipe = info.mode == INST_WHOLE;
        win_text(win, 6, rowy(0), "Na pewno?", wipe ? C_RED : C_HEAD);
        if (wipe) {
            win_text(win, 6, rowy(2),
                     "Zaraz skasuje CALY ten dysk i zrobie go na nowo.",
                     C_TEXT);
            win_text(win, 6, rowy(3),
                     "Tego sie nie da cofnac.", C_RED);
            win_text(win, 6, rowy(5),
                     "Wpisz KASUJ (duzymi literami) i nacisnij Enter:",
                     C_TEXT);
        } else {
        win_text(win, 6, rowy(2),
                 "Zaraz zapisze pliki na partycji EFI tej maszyny.", C_TEXT);
        win_text(win, 6, rowy(3),
                 "Nic innego na dysku sie nie zmieni.", C_TEXT);
        win_text(win, 6, rowy(5),
                 "Wpisz TAK (duzymi literami) i nacisnij Enter:", C_TEXT);
        }
        win_rect(win, 6, rowy(7) - 3, 120, 14, C_BLACK);
        win_text(win, 10, rowy(7), typed, C_WHITE);   /* a black field */
        if (note[0])
            win_text(win, 6, rowy(9), note, C_RED);
        button(win, "Nie instaluj", C_BLUE);
        return;
    }

    if (state == S_PASS) {
        char stars[24];
        int n = plen < 20 ? plen : 20;
        win_text(win, 6, rowy(0), "Trzeba byc adminem", C_HEAD);
        win_text(win, 6, rowy(2),
                 "Jadro nie pozwolilo zapisac. Podaj haslo admina", C_TEXT);
        win_text(win, 6, rowy(3), "i nacisnij Enter:", C_TEXT);
        win_rect(win, 6, rowy(5) - 3, 160, 14, C_BLACK);
        for (int i = 0; i < n; i++)
            stars[i] = '*';
        stars[n] = '\0';
        win_text(win, 10, rowy(5), stars, C_WHITE);   /* a black field */
        if (note[0])
            win_text(win, 6, rowy(7), note, C_RED);
        button(win, "Rezygnuje", C_BLUE);
        return;
    }

    if (state == S_WORK) {
        int at = add(line, 0, "krok ");
        at = addnum(line, at, (uint32_t)step + 1);
        at = add(line, at, " z ");
        at = addnum(line, at, info.steps);
        win_text(win, 6, rowy(0), "Zapisuje...", C_HEAD);
        win_text(win, 6, rowy(2), line, C_TEXT);
        win_text(win, 6, rowy(3), step_name(step), C_TEXT);
        win_text(win, 6, rowy(5), "Nie wylaczaj maszyny.", C_RED);
        draw_bar(win, step, (int)info.steps);
        return;
    }

    if (state == S_DONE) {
        win_text(win, 6, rowy(0), "Gotowe", C_HEAD);
        if (info.fallback_free) {
            win_text(win, 6, rowy(2),
                     "Ta maszyna uruchomi ProtonOS-a z dysku.", C_TEXT);
            win_text(win, 6, rowy(3),
                     "Mozesz wyjac pendrive i zrestartowac.", C_TEXT);
        } else {
            win_text(win, 6, rowy(2), "Pliki sa na partycji EFI.", C_TEXT);
            win_text(win, 6, rowy(3),
                     "Dodaj w firmwarze wpis wskazujacy:", C_TEXT);
            win_text(win, 6, rowy(4), STEP_NAME[1], C_HEAD);
        }
        win_text(win, 6, rowy(6), "Nic cudzego nie zostalo ruszone.",
                 C_TEXT);
        draw_bar(win, (int)info.steps, (int)info.steps);
        button(win, "Zamknij", C_BLUE);
        return;
    }

    /* S_FAIL */
    win_text(win, 6, rowy(0), "Nie udalo sie", C_RED);
    win_text(win, 6, rowy(2), "Zatrzymalem sie na:", C_TEXT);
    win_text(win, 6, rowy(3), step_name(failed_at), C_TEXT);
    win_text(win, 6, rowy(5), "Nic poza tym, co powyzej, nie zostalo",
             C_TEXT);
    win_text(win, 6, rowy(6),
             "zapisane - a nic cudzego nie zostalo ruszone.", C_TEXT);
    draw_bar(win, failed_at, (int)info.steps);
    button(win, "Sprawdz jeszcze raz", C_BLUE);
}

/* --- doing it -------------------------------------------------------- */

static void look(int mode, int disk)
{
    sys_install_look(mode, disk, &info);
    state = S_LOOK;
    step = 0;
    failed_at = -1;
    tlen = 0;
    typed[0] = '\0';
    note[0] = '\0';
}

/* One step, and what its answer means. Failing is one thing; being
   told no is another, and the answer to the second is a password
   rather than a retry. */
static void one_step(void)
{
    int rc = sys_install_step(step);
    if (rc == -2) {
        plen = 0;
        pass[0] = '\0';
        ecopy(note, "", sizeof(note));
        state = S_PASS;
        return;
    }
    if (rc < 0) {
        failed_at = step;
        state = S_FAIL;
        return;
    }
    step++;
    if (step >= (int)info.steps)
        state = S_DONE;
}

static void try_password(void)
{
    pass[plen] = '\0';
    if (!plen) {
        ecopy(note, "puste haslo", sizeof(note));
        return;
    }
    if (sys_auth(pass) != 0) {
        for (int i = 0; i < plen; i++)
            pass[i] = 0;
        plen = 0;
        ecopy(note, "zle haslo", sizeof(note));
        return;
    }
    for (int i = 0; i < plen; i++)
        pass[i] = 0;
    plen = 0;
    note[0] = '\0';
    we_authed = 1;
    state = S_WORK;              /* carry on from the step that was refused */
}

void _start(void)
{
    int win = (int)win_create(W0, H0, "Instalacja");
    if (win < 0)
        sys_exit(1);

    look(INST_ALONGSIDE, -1);
    draw(win);

    for (;;) {
        struct win_event ev;
        int again = 0;

        while (win_poll(win, &ev) == 1) {
            again = 1;
            if (ev.type == WEV_CLOSE) {
                /* Not in the middle of writing. A window that vanishes
                   halfway through leaves a file half on the disk, and
                   the two seconds the desktop waits are not enough to
                   finish - so we say we are staying and keep going. */
                if (state == S_WORK) {
                    win_keep(win);
                    continue;
                }
                if (we_authed)
                    sys_drop();
                sys_exit(0);
            }
            if (ev.type == WEV_SIZE) {
                cw = ev.x;
                ch = ev.y;
                continue;
            }
            if (ev.type == WEV_CLICK) {
                /* The disk line, when there is more than one disk to
                   choose between. Before the buttons, because it is
                   inside the text and a button is not. */
                if (state == S_LOOK && info.disks > 1 &&
                    ev.y >= rowy(DISK_ROW) && ev.y < rowy(DISK_ROW) + 8) {
                    look((int)info.mode,
                         (int)((info.disk + 1) % info.disks));
                    again = 1;
                    continue;
                }
                int b = in_button(ev.x, ev.y);
                if (!b)
                    continue;
                if (state == S_LOOK) {
                    /* The second button always means the other way of
                       doing it: from the ordinary plan it goes to the
                       whole disk, and from the whole disk back. */
                    if (info.mode == INST_WHOLE) {
                        if (b == 1)
                            look(INST_ALONGSIDE, (int)info.disk);
                        else
                            state = S_WORD;
                    } else if (b == 2) {
                        look(INST_WHOLE, (int)info.disk);
                    } else if (info.ok) {
                        state = S_WORD;
                    } else {
                        look(INST_ALONGSIDE, -1);
                    }
                    tlen = 0;
                    typed[0] = '\0';
                    note[0] = '\0';
                } else if (state == S_FAIL) {
                    look(INST_ALONGSIDE, -1);
                } else if (state == S_WORD) {
                    look((int)info.mode, (int)info.disk);           /* "Nie instaluj" / "Wroc" */
                } else if (state == S_PASS) {
                    for (int i = 0; i < plen; i++)
                        pass[i] = 0;
                    plen = 0;
                    look(INST_ALONGSIDE, -1);
                } else if (state == S_DONE) {
                    /* Only a right we asked for is a right to give
                       back: somebody who was an admin before this
                       program started still is one after it. */
                    if (we_authed)
                        sys_drop();
                    sys_exit(0);
                }
                continue;
            }
            if (ev.type == WEV_KEY) {
                char c = (char)ev.val;
                if (state == S_WORD) {
                    if (c == '\n' || c == '\r') {
                        /* A different word for the way that destroys
                           things: TAK is what one says to a question,
                           KASUJ is what one says to this. */
                        const char *want = info.mode == INST_WHOLE
                                           ? "KASUJ" : "TAK";
                        int same = 1, i = 0;
                        typed[tlen] = '\0';
                        for (; want[i]; i++)
                            if (typed[i] != want[i])
                                same = 0;
                        if (typed[i])
                            same = 0;
                        if (same) {
                            note[0] = '\0';
                            state = S_WORK;
                        } else {
                            ecopy(note, info.mode == INST_WHOLE
                                        ? "to nie jest slowo KASUJ"
                                        : "to nie jest slowo TAK",
                                  sizeof(note));
                            tlen = 0;
                            typed[0] = '\0';
                        }
                    } else if (c == '\b') {
                        if (tlen)
                            typed[--tlen] = '\0';
                    } else if (c >= 0x20 && c < 0x7F &&
                               tlen < (int)sizeof(typed) - 1) {
                        typed[tlen++] = c;
                        typed[tlen] = '\0';
                    }
                } else if (state == S_PASS) {
                    if (c == '\n' || c == '\r')
                        try_password();
                    else if (c == '\b') {
                        if (plen)
                            pass[--plen] = 0;
                    } else if (c >= 0x20 && c < 0x7F &&
                               plen < (int)sizeof(pass) - 1) {
                        pass[plen++] = c;
                    }
                }
                continue;
            }
        }

        /* One step per turn of the loop, so that the bar moves and the
           name under it is the file actually being written */
        if (state == S_WORK) {
            draw(win);
            one_step();
            again = 1;
        }

        if (again)
            draw(win);
        sys_sleep(60);
    }
}
