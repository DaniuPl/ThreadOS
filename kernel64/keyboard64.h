#ifndef PROTONOS_KEYBOARD64_H
#define PROTONOS_KEYBOARD64_H

/* Installs the IRQ1 handler and unmasks the keyboard interrupt */
void keyboard64_init(void);

/* Waits for a key and returns it (sleeps while there is nothing) */
char keyboard64_getchar(void);

/* The desktop intercepts all keys (in graphics mode there is no text
   console for them to reach) */
void keyboard64_set_override(void (*handler)(char c));
void keyboard64_set_fkey_override(void (*handler)(int fkey));

/* Whom to send SIGINT to after Ctrl+C (-1 = nobody). It concerns the
   console the calling task types on. */
void keyboard64_set_intr_target(int id);

/* A character and a console switch from a source other than PS/2 (a
   USB keyboard) */
void keyboard64_inject(char c);
void keyboard64_inject_switch(int vc);

/* A read for a program in ring 3: it blocks until the first character
   and returns how many it took; -1 = a signal interrupted the wait (no
   echo - the echo is done by the reader) */
int keyboard64_read(char *out, uint32_t len);

/* Is anything waiting in the queue */
int keyboard64_pending(void);

#endif
