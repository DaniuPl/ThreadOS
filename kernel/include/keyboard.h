#ifndef THREADOS_KEYBOARD_H
#define THREADOS_KEYBOARD_H

/* Keys with no ASCII equivalent (arrows, PgUp/PgDn, Home/End) are
   delivered as control codes - programs recognise them by value and
   the shell ignores them. */
#define KEY_UP    0x11
#define KEY_DOWN  0x12
#define KEY_LEFT  0x13
#define KEY_RIGHT 0x14
#define KEY_PGUP  0x15
#define KEY_PGDN  0x16
#define KEY_HOME  0x17
#define KEY_END   0x18

/* Alt+Tab: not a character either, and not for a program - it says
   'the next window'. The desktop takes it before anything else does;
   where there is no desktop, nothing reads it. */
#define KEY_NEXTWIN 0x19

/* The clipboard, from the keyboard: Ctrl+Insert copies what is being
   typed, Shift+Insert pastes it. The oldest pair of keys for it, and
   the one that does not collide with Ctrl+C - which here means 'stop
   that program' and nothing else. */
#define KEY_COPY  0x1A
#define KEY_PASTE 0x1B

/* Ctrl+Alt+Left/Right change the desktop; with Shift the window that
   has the focus comes along. In text mode nothing reads them. */
#define KEY_DESK_PREV      0x1C
#define KEY_DESK_NEXT      0x1D
#define KEY_DESK_TAKE_PREV 0x1E
#define KEY_DESK_TAKE_NEXT 0x1F
/* Ctrl+C: not a character but a request to interrupt. The driver turns
   it into a SIGINT for the process holding the console (see
   kernel64/keyboard64.c). */
/* Ctrl+S and Ctrl+O. The other Ctrl+letter combinations arrive as the
   letter itself, which is what a shell wants; these two do not, because
   a program that writes files has to be able to tell "save" from "s".
   The kernel knows no more than that the keys were pressed - what to do
   with them is the program's business. */
#define KEY_SHOT  0x10      /* Print Screen - save what is on the screen */
#define KEY_SEARCH 0x01     /* Ctrl+Space - the desktop's search */
#define KEY_LOCK   0x02     /* Ctrl+Alt+L - lock the screen */
#define KEY_MENU   0x0F     /* the Windows key - the desktop's menu */
/* The loudness keys a laptop has on Fn (PS/2 E0 30, E0 2E, E0 20):
   the desktop turns the sound up, down or off and shows by how much */
#define KEY_VOLUP   0x0B
#define KEY_VOLDOWN 0x0C
#define KEY_MUTE    0x7F
#define KEY_UNDO  0x0E      /* Ctrl+Z - take that back */
#define KEY_FIND  0x07      /* Ctrl+F - "find", the third of them */
#define KEY_SAVE  0x05
#define KEY_OPEN  0x06
#define KEY_INTR  0x03

/* Ctrl+D: the end of what is being typed. Not a character either - it
   is what closes the input of a program that is reading. */
#define KEY_EOF   0x04

/* Somebody who takes characters one at a time */
typedef void (*keyboard_char_handler_t)(char c);

/* Handler of the visible console - the desktop hands keys to it when
   no window has focus */
keyboard_char_handler_t keyboard_console_handler(void);

/* The desktop grabs all keys (graphics mode has no consoles) */
void keyboard_set_override(keyboard_char_handler_t handler);

/* And the function keys with Alt, which in text mode switch consoles
   and in graphics mode have nothing to switch. The number is counted
   from zero, so Alt+F4 arrives as 3. */
typedef void (*keyboard_fkey_handler_t)(int fkey);
void keyboard_set_fkey_override(keyboard_fkey_handler_t handler);

/* A character from a source other than PS/2 (a USB keyboard) - the
   road onward is the same */
void keyboard_inject(char c);
void keyboard_inject_switch(int vc);

#endif
