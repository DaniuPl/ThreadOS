#ifndef PROTONOS_KEYBOARD_H
#define PROTONOS_KEYBOARD_H

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

/* Ctrl+C: not a character but a request to interrupt. The driver turns
   it into a SIGINT for the process holding the console (see keyboard.c). */
#define KEY_INTR  0x03

/* Installs the IRQ1 handler and unmasks the keyboard interrupt */
void keyboard_init(void);

/* The character sink of a given virtual console (that console's
   shell). A character goes there only when no process holds the
   console. */
typedef void (*keyboard_char_handler_t)(char c);
void keyboard_set_handler(int vc, keyboard_char_handler_t handler);

/* Handler of the visible console - the desktop hands keys to it when
   no window has focus */
keyboard_char_handler_t keyboard_console_handler(void);

/* The desktop grabs all keys (graphics mode has no consoles) */
void keyboard_set_override(keyboard_char_handler_t handler);

/* A character from a source other than PS/2 (a USB keyboard) - the
   road onward is the same */
void keyboard_inject(char c);
void keyboard_inject_switch(int vc);

/* Blocking read for processes (the read syscall). The current task
   takes the console over until it dies; returns >=1 characters read
   (without echo - the reader does the echo) or -1 when the wait was
   interrupted by a signal (the process is about to enter a handler). */
int keyboard_read(char *buf, uint32_t len);

#endif
